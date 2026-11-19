#!/usr/bin/env python3
"""The one DriveLink version, from the root VERSION file.

    python tools/version.py            # the full version of this checkout
    python tools/version.py --base     # the VERSION file's semver
    python tools/version.py --check    # every consumer names the same version

The full version is the base at a clean checkout of tag v<base>, otherwise
<base>+g<sha7>, with ".dirty" when tracked files changed. Without git it is the
base. A DRIVELINK_VERSION_FULL environment variable (<base> or <base>+<suffix>)
wins: validate.py computes the version on Windows and passes it to builds in
WSL, whose git cannot read a Windows worktree. cmake/DriveLinkVersion.cmake
applies the same rules for CMake builds (firmware PROJECT_VER, the host).
Standard library only.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SEMVER = re.compile(r'^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$')

# Files that must carry the base version literally (CMake reads VERSION itself).
PACKAGE_XML = ['protocol/package.xml', 'host/libdrivelink/package.xml']
# CMake projects that must take their version from cmake/DriveLinkVersion.cmake.
CMAKE_PROJECTS = ['protocol/CMakeLists.txt', 'host/libdrivelink/CMakeLists.txt',
                  'host/superbuild/CMakeLists.txt']
CMAKE_PROJECT_VER = ['firmware/CMakeLists.txt', 'firmware/test/uart_comm/CMakeLists.txt']


def base(root=ROOT):
    text = (root / 'VERSION').read_text(encoding='utf-8').strip()
    if not SEMVER.match(text):
        raise ValueError(f'VERSION must be MAJOR.MINOR.PATCH, found {text!r}')
    return text


def _git(root, *args):
    try:
        done = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    except OSError:
        return None
    return done.stdout.strip() if done.returncode == 0 else None


def full(root=ROOT, git=_git, environ=os.environ):
    """The base, or base+g<sha7>[.dirty] away from a clean v<base> tag."""
    version = base(root)
    given = environ.get('DRIVELINK_VERSION_FULL')
    if given:
        if not re.fullmatch(re.escape(version) + r'(\+[0-9A-Za-z.]+)?', given):
            raise ValueError(f'DRIVELINK_VERSION_FULL {given!r} does not extend VERSION {version}')
        return given
    sha = git(root, 'rev-parse', '--short=7', 'HEAD')
    if not sha:
        return version
    dirty = git(root, 'status', '--porcelain', '--untracked-files=no')
    tagged = git(root, 'tag', '--points-at', 'HEAD') or ''
    if dirty == '' and f'v{version}' in tagged.split():
        return version
    return f'{version}+g{sha}' + ('.dirty' if dirty else '')


def check(root=ROOT):
    """Problems as readable lines; empty when every consumer agrees."""
    try:
        version = base(root)
    except (OSError, ValueError) as error:
        return [str(error)]
    problems = []
    for path in PACKAGE_XML:
        found = re.findall(r'<version>([^<]*)</version>', (root / path).read_text(encoding='utf-8'))
        if found != [version]:
            problems.append(f'{path}: <version> is {found or "missing"}, VERSION is {version}')
    for path in CMAKE_PROJECTS:
        text = (root / path).read_text(encoding='utf-8')
        if 'DriveLinkVersion.cmake' not in text or not re.search(r'project\([^)]*VERSION \$\{DRIVELINK_VERSION\}', text):
            problems.append(f'{path}: project() must take VERSION ${{DRIVELINK_VERSION}} from DriveLinkVersion.cmake')
        if re.search(r'VERSION \d+\.\d+\.\d+ COMPATIBILITY', text):
            problems.append(f'{path}: a literal package version; use ${{DRIVELINK_VERSION}}')
    for path in CMAKE_PROJECT_VER:
        text = (root / path).read_text(encoding='utf-8')
        if 'DriveLinkVersion.cmake' not in text or 'PROJECT_VER "${DRIVELINK_VERSION_FULL}"' not in text:
            problems.append(f'{path}: PROJECT_VER must come from DriveLinkVersion.cmake')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--base', action='store_true', help='print the VERSION file')
    group.add_argument('--check', action='store_true', help='every consumer names VERSION')
    args = parser.parse_args(argv)
    if args.check:
        problems = check()
        for line in problems:
            print(line, file=sys.stderr)
        if not problems:
            print(f'version {base()}: every consumer matches')
        return 1 if problems else 0
    print(base() if args.base else full())
    return 0


if __name__ == '__main__':
    sys.exit(main())
