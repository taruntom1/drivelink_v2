"""The one-version rules of tools/version.py, with a fake git and a fake tree."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import version

ROOT = Path(__file__).resolve().parents[2]


def fake_git(sha='abc1234', dirty='', tags=''):
    answers = {'rev-parse': sha, 'status': dirty, 'tag': tags}
    return lambda root, *args: answers[args[0]]


class VersionTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        files = ['VERSION', *version.PACKAGE_XML, *version.CMAKE_PROJECTS, *version.CMAKE_PROJECT_VER]
        for name in files:
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, self.root / name)

    def write(self, name, text):
        (self.root / name).write_text(text, encoding='utf-8')

    def test_the_repository_agrees(self):
        self.assertEqual(version.check(ROOT), [])

    def test_base_must_be_semver(self):
        for text in ['0.1', 'v0.1.0', '01.2.3', '0.1.0-rc1', '']:
            self.write('VERSION', text + '\n')
            with self.assertRaises(ValueError):
                version.base(self.root)
        self.write('VERSION', ' 2.10.0 \n')
        self.assertEqual(version.base(self.root), '2.10.0')

    def test_full_version_suffix(self):
        self.write('VERSION', '0.1.0\n')
        cases = [
            (dict(), '0.1.0+gabc1234'),
            (dict(dirty=' M file'), '0.1.0+gabc1234.dirty'),
            (dict(tags='v0.1.0'), '0.1.0'),
            (dict(tags='other\nv0.1.0'), '0.1.0'),
            (dict(tags='v0.1.0', dirty=' M file'), '0.1.0+gabc1234.dirty'),
            (dict(tags='v0.1.1'), '0.1.0+gabc1234'),
            (dict(tags='v0.1.0-rc'), '0.1.0+gabc1234'),
        ]
        for answers, expected in cases:
            self.assertEqual(version.full(self.root, git=fake_git(**answers)), expected, answers)
        self.assertEqual(version.full(self.root, git=lambda root, *args: None), '0.1.0')
        # The longest form fits esp_app_desc_t.version (31 characters).
        self.assertLessEqual(len(version.full(self.root, git=fake_git(dirty='x'))), 31)

    def test_check_reports_each_mismatch(self):
        self.assertEqual(version.check(self.root), [])
        self.write('VERSION', '0.2.0\n')
        problems = version.check(self.root)
        self.assertEqual(len(problems), len(version.PACKAGE_XML))
        self.assertTrue(all('<version>' in line for line in problems))

    def test_check_refuses_literal_cmake_versions(self):
        path = version.CMAKE_PROJECTS[1]
        text = (self.root / path).read_text(encoding='utf-8')
        self.write(path, text.replace('VERSION ${DRIVELINK_VERSION} LANGUAGES', 'VERSION 0.1.0 LANGUAGES'))
        self.assertEqual(len(version.check(self.root)), 1)
        path = version.CMAKE_PROJECT_VER[0]
        text = (self.root / path).read_text(encoding='utf-8')
        self.write(path, text.replace('set(PROJECT_VER "${DRIVELINK_VERSION_FULL}")', 'set(PROJECT_VER "1.0")'))
        self.assertEqual(len(version.check(self.root)), 2)

    def test_bad_version_file_is_one_problem(self):
        self.write('VERSION', 'one\n')
        self.assertEqual(len(version.check(self.root)), 1)


if __name__ == '__main__':
    unittest.main()
