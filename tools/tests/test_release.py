"""Tests for tools/release.py."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release
import version as dl_version


def quiet(call, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return call(*args, **kwargs)


def run_git(cwd, *args):
    subprocess.run(['git', '-C', str(cwd), *args], check=True, capture_output=True, text=True)


class ReleaseRepoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.remote = base / 'origin.git'
        self.root = base / 'repo'
        self.remote.mkdir()
        self.root.mkdir()
        run_git(self.remote, 'init', '--bare', '-b', 'main')
        run_git(self.root, 'init', '-b', 'main')
        run_git(self.root, 'config', 'user.name', 'Test User')
        run_git(self.root, 'config', 'user.email', 'test@example.com')
        run_git(self.root, 'remote', 'add', 'origin', str(self.remote))

        self.write('VERSION', '0.2.1\n')
        for rel in dl_version.PACKAGE_XML:
            self.write(rel, '<package><version>0.2.1</version></package>\n')
        for rel in dl_version.CMAKE_PROJECTS:
            self.write(rel, 'include(DriveLinkVersion.cmake)\nproject(x VERSION ${DRIVELINK_VERSION})\n')
        for rel in dl_version.CMAKE_PROJECT_VER:
            self.write(rel, 'include(DriveLinkVersion.cmake)\nset(PROJECT_VER "${DRIVELINK_VERSION_FULL}")\n')

        run_git(self.root, 'add', '-A')
        run_git(self.root, 'commit', '-m', 'Initial commit')
        run_git(self.root, 'push', '-u', 'origin', 'main')

    def write(self, rel, content):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')

    def test_normalize_version(self):
        self.assertEqual(release.normalize_version('0.2.2'), '0.2.2')
        self.assertEqual(release.normalize_version('v1.0.0'), '1.0.0')
        self.assertEqual(release.normalize_version(' V0.3.1 '), '0.3.1')
        for bad in ('', '0.2', '0.2.1-rc1', '0.02.1', 'abc'):
            with self.assertRaises(release.ReleaseError):
                release.normalize_version(bad)

    def test_rejects_same_or_older_version(self):
        for candidate in ('0.2.1', '0.2.0', '0.1.9'):
            args = release.arguments([candidate, '--validate', 'none', '--yes'])
            with self.assertRaises(release.ReleaseError):
                quiet(release.execute, args, root=self.root)

    def test_rejects_dirty_tree(self):
        self.write('protocol/package.xml', '<package><version>0.2.1</version><!-- dirty --></package>\n')
        args = release.arguments(['0.2.2', '--validate', 'none', '--yes'])
        with self.assertRaises(release.ReleaseError):
            quiet(release.execute, args, root=self.root)

    def test_rejects_non_main_branch_unless_allowed(self):
        run_git(self.root, 'checkout', '-b', 'feature')
        args = release.arguments(['0.2.2', '--validate', 'none', '--yes', '--no-push'])
        with self.assertRaises(release.ReleaseError):
            quiet(release.execute, args, root=self.root)
        args = release.arguments(['0.2.2', '--validate', 'none', '--yes', '--no-push', '--allow-branch'])
        self.assertEqual(quiet(release.execute, args, root=self.root), 0)

    def test_rejects_existing_tag(self):
        run_git(self.root, 'tag', '-a', 'v0.2.2', '-m', 'DriveLink 0.2.2')
        args = release.arguments(['0.2.2', '--validate', 'none', '--yes', '--no-push'])
        with self.assertRaises(release.ReleaseError):
            quiet(release.execute, args, root=self.root)

    def test_dry_run_makes_no_changes(self):
        before = release.git(self.root, 'rev-parse', 'HEAD').stdout.strip()
        args = release.arguments(['0.2.2', '--dry-run'])
        self.assertEqual(quiet(release.execute, args, root=self.root), 0)
        self.assertEqual(dl_version.base(self.root), '0.2.1')
        self.assertEqual(release.git(self.root, 'rev-parse', 'HEAD').stdout.strip(), before)
        self.assertEqual(release.git(self.root, 'tag', '--list').stdout.strip(), '')

    def test_cancel_at_prompt_makes_no_changes(self):
        args = release.arguments(['0.2.2', '--validate', 'none'])
        code = quiet(release.execute, args, root=self.root, prompt_fn=lambda _: False)
        self.assertEqual(code, 130)
        self.assertEqual(dl_version.base(self.root), '0.2.1')

    def test_validation_failure_reverts_version_files(self):
        args = release.arguments(['0.2.2', '--validate', 'gen', '--yes', '--no-push'])
        with self.assertRaises(release.ReleaseError):
            quiet(release.execute, args, root=self.root, validate_fn=lambda _r, _m: 2)
        self.assertEqual(dl_version.base(self.root), '0.2.1')
        self.assertEqual(dl_version.check(self.root), [])
        self.assertEqual(release.git(self.root, 'status', '--porcelain').stdout.strip(), '')

    def test_full_release_commits_tags_and_pushes(self):
        seen = []
        args = release.arguments(['v0.2.2', '--validate', 'gen', '--yes'])
        code = quiet(release.execute, args, root=self.root, validate_fn=lambda r, m: seen.append((r, m)) or 0)
        self.assertEqual(code, 0)
        self.assertEqual(seen, [(self.root, 'gen')])
        self.assertEqual(dl_version.base(self.root), '0.2.2')
        self.assertEqual(dl_version.full(self.root), '0.2.2')
        self.assertEqual(dl_version.check(self.root), [])
        subject = release.git(self.root, 'log', '-1', '--format=%s').stdout.strip()
        self.assertEqual(subject, 'Prepare DriveLink 0.2.2 for release')
        tag_msg = release.git(self.root, 'tag', '-n1', 'v0.2.2').stdout.strip()
        self.assertIn('DriveLink 0.2.2', tag_msg)
        remote_main = release.git(self.remote, 'rev-parse', 'refs/heads/main').stdout.strip()
        local_head = release.git(self.root, 'rev-parse', 'HEAD').stdout.strip()
        self.assertEqual(remote_main, local_head)
        remote_tag = release.git(self.remote, 'rev-parse', 'refs/tags/v0.2.2^{commit}').stdout.strip()
        self.assertEqual(remote_tag, local_head)


if __name__ == '__main__':
    unittest.main()
