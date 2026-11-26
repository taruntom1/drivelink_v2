#!/usr/bin/env python3
"""Automate a versioned DriveLink release.

    python tools/release.py 0.2.2
    python tools/release.py 0.2.2 --validate full
    python tools/release.py 0.2.2 --dry-run

Updates VERSION and the four package.xml files, checks version consistency,
runs the selected local validation (gen by default), commits, creates the
annotated tag v<version>, and pushes main and the tag to origin. CI then runs
every check on the tag and publishes the GitHub Release, apt repository, and
user guide.

Exit codes: 0 ok, 1 failed or refused, 130 cancelled.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

import version as dl_version

ROOT = Path(__file__).resolve().parents[1]
VALIDATE_MODES = ('gen', 'quick', 'full', 'none')
VERSION_TAG_RE = re.compile(r'<version>[^<]*</version>')


class ReleaseError(Exception):
    """A clean refusal or failure message for the user."""


def normalize_version(raw):
    text = raw.strip()
    if text.startswith(('v', 'V')):
        text = text[1:]
    if not dl_version.SEMVER.match(text):
        raise ReleaseError(f'version must be MAJOR.MINOR.PATCH (got {raw!r})')
    return text


def semver_tuple(text):
    return tuple(int(part) for part in text.split('.'))


def git(root, *args, check=True):
    done = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    if check and done.returncode != 0:
        detail = (done.stderr or done.stdout).strip()
        raise ReleaseError(f"git {' '.join(args)} failed: {detail}")
    return done


def update_version_files(root, new_version):
    version_path = root / 'VERSION'
    version_path.write_text(f'{new_version}\n', encoding='utf-8', newline='\n')
    for rel in dl_version.PACKAGE_XML:
        path = root / rel
        text = path.read_text(encoding='utf-8')
        updated, count = VERSION_TAG_RE.subn(f'<version>{new_version}</version>', text)
        if count != 1:
            raise ReleaseError(f'{rel}: expected 1 <version> tag, found {count}')
        path.write_text(updated, encoding='utf-8', newline='\n')


def revert_version_files(root):
    files = ['VERSION', *dl_version.PACKAGE_XML]
    git(root, 'checkout', '--', *files, check=False)


def run_validation(root, mode):
    if mode == 'none':
        return 0
    cmd = [sys.executable, str(root / 'tools' / 'validate.py')]
    if mode == 'gen':
        cmd.extend(['--only', 'gen'])
    else:
        cmd.append(mode)
    print('$ ' + ' '.join(cmd), file=sys.stderr, flush=True)
    return subprocess.run(cmd, cwd=root).returncode


def preflight(root, new_version, remote, allow_branch, no_push):
    dirty = git(root, 'status', '--porcelain', '--untracked-files=no').stdout.strip()
    if dirty:
        raise ReleaseError('working tree has uncommitted changes; commit or stash them first')

    try:
        current = dl_version.base(root)
    except (OSError, ValueError) as error:
        raise ReleaseError(str(error)) from error
    if semver_tuple(new_version) <= semver_tuple(current):
        raise ReleaseError(f'new version {new_version} must be greater than current VERSION {current}')

    branch = git(root, 'rev-parse', '--abbrev-ref', 'HEAD').stdout.strip()
    if not allow_branch and branch != 'main':
        raise ReleaseError(f'current branch is {branch!r}; switch to main or pass --allow-branch')

    tag = f'v{new_version}'
    if git(root, 'rev-parse', '--verify', '--quiet', f'refs/tags/{tag}', check=False).returncode == 0:
        raise ReleaseError(f'tag {tag} already exists locally')

    head = git(root, 'rev-parse', 'HEAD').stdout.strip()
    if not no_push:
        git(root, 'fetch', remote, 'main', '--tags')
        remote_main = git(root, 'rev-parse', '--verify', f'{remote}/main', check=False)
        if remote_main.returncode == 0 and remote_main.stdout.strip() != head:
            raise ReleaseError(
                f'HEAD ({head[:7]}) does not match {remote}/main ({remote_main.stdout.strip()[:7]}); '
                f'pull or merge {remote}/main first'
            )
        remote_tag = git(root, 'ls-remote', '--tags', remote, f'refs/tags/{tag}').stdout.strip()
        if remote_tag:
            raise ReleaseError(f'tag {tag} already exists on {remote}')

    return current, branch, head


def confirm(prompt):
    try:
        answer = input(prompt).strip().lower()
    except EOFError:
        return False
    return answer in ('y', 'yes')


def execute(args, root=ROOT, prompt_fn=confirm, validate_fn=run_validation):
    new_version = normalize_version(args.version)
    tag = f'v{new_version}'
    current, branch, head = preflight(root, new_version, args.remote, args.allow_branch, args.no_push)

    files = ['VERSION', *dl_version.PACKAGE_XML]
    print(f'Release plan for DriveLink {new_version}:')
    print(f'  version:    {current} -> {new_version}')
    print(f'  branch:     {branch} ({head[:7]})')
    print(f'  files:      {", ".join(files)}')
    print(f'  validation: {args.validate}')
    print(f'  tag:        {tag} ("DriveLink {new_version}")')
    print(f'  push:       {"no (--no-push)" if args.no_push else f"{args.remote} (main and {tag})"}')

    if args.dry_run:
        print('Dry run: no files modified, nothing committed or pushed.')
        return 0

    if not args.yes and not prompt_fn(f'Proceed with release {tag}? [y/N]: '):
        print('Cancelled.', file=sys.stderr)
        return 130

    update_version_files(root, new_version)
    problems = dl_version.check(root)
    if problems:
        revert_version_files(root)
        raise ReleaseError('version check failed:\n  ' + '\n  '.join(problems))

    code = validate_fn(root, args.validate)
    if code != 0:
        revert_version_files(root)
        raise ReleaseError(f'validation ({args.validate}) failed with exit code {code}; version edits reverted')

    git(root, 'add', *files)
    git(root, 'commit', '-m', f'Prepare DriveLink {new_version} for release')
    git(root, 'tag', '-a', tag, '-m', f'DriveLink {new_version}')

    resolved = dl_version.full(root)
    if resolved != new_version:
        raise ReleaseError(f'expected tagged version {new_version}, got {resolved}')

    if not args.no_push:
        git(root, 'push', args.remote, 'HEAD:main')
        git(root, 'push', args.remote, tag)
        print(f'Released {tag} and pushed to {args.remote}. CI will publish the release once all checks pass.')
    else:
        print(f'Created local commit and tag {tag} (--no-push).')
    return 0


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('version', help='target semver MAJOR.MINOR.PATCH (optional leading v)')
    parser.add_argument('--validate', choices=VALIDATE_MODES, default='gen',
                        help='local validation to run before committing (default: gen)')
    parser.add_argument('--remote', default='origin', help='git remote to fetch and push (default: origin)')
    parser.add_argument('--allow-branch', action='store_true',
                        help='allow releasing from a branch other than main (still pushes HEAD:main)')
    parser.add_argument('--no-push', action='store_true',
                        help='commit and tag locally without pushing to the remote')
    parser.add_argument('--dry-run', action='store_true',
                        help='run preflight checks and print the release plan without making changes')
    parser.add_argument('--yes', '-y', action='store_true', help='skip the interactive confirmation prompt')
    return parser.parse_args(argv)


def main(argv=None):
    args = arguments(sys.argv[1:] if argv is None else argv)
    try:
        return execute(args)
    except ReleaseError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
