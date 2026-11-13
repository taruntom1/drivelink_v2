"""Tests for the pure parts of tools/validate.py."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import validate  # noqa: E402


class SelectTest(unittest.TestCase):
    def test_quick_is_the_fast_checks_for_this_os(self):
        self.assertEqual(validate.select('quick', None, None, windows=True)[0],
                         ['gen', 'native', 'host-win'])
        self.assertEqual(validate.select('quick', None, None, windows=False)[0],
                         ['gen', 'native', 'host-linux'])

    def test_full_is_every_check_of_this_os(self):
        chosen, left_out = validate.select('full', None, None, windows=True)
        self.assertEqual(chosen, [check.name for check in validate.CHECKS])
        self.assertEqual(left_out, [])
        self.assertNotIn('host-win', validate.select('full', None, None, windows=False)[0])

    def test_only_and_skip_keep_the_run_order_and_report_the_rest(self):
        chosen, left_out = validate.select('quick', ['fw', 'gen'], None, windows=True)
        self.assertEqual(chosen, ['gen', 'fw'])
        self.assertIn('fw-nolog', left_out)
        chosen, left_out = validate.select('full', None, ['host-linux'], windows=True)
        self.assertNotIn('host-linux', chosen)
        self.assertEqual(left_out, ['host-linux'])

    def test_an_unknown_or_foreign_check_is_an_error(self):
        with self.assertRaises(SystemExit):
            validate.select('full', ['nope'], None)
        with self.assertRaises(SystemExit):
            validate.select('full', ['host-win'], None, windows=False)


class ParsingTest(unittest.TestCase):
    def test_warning_lines_match_what_ci_grepped(self):
        output = ('main.cpp:3:9: warning: unused variable [-Wunused-variable]\n'
                  'CMake Warning at CMakeLists.txt:4 (message):\n'
                  '[12/40] Building CXX object main.cpp.obj\n')
        self.assertEqual(validate.warning_lines(output),
                         ['main.cpp:3:9: warning: unused variable [-Wunused-variable]'])

    def test_parse_env_reads_only_what_follows_the_marker(self):
        env = validate.parse_env('noise=1\n--m--\nPath=C:\\a;C:\\b\nIDF_PATH=C:\\idf\nA=b=c\n', '--m--')
        self.assertEqual(env, {'PATH': 'C:\\a;C:\\b', 'IDF_PATH': 'C:\\idf', 'A': 'b=c'})


class FingerprintTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        for name, value in (('ROOT', self.root), ('CI', False)):
            patch = mock.patch.object(validate, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        self.git('init', '-q')
        (self.root / 'plans').mkdir()
        for name in ('a.cpp', 'README.md', 'plans/status.md'):
            (self.root / name).write_text('first')
        self.code = validate.Check('code', '', None)
        self.docs = validate.Check('docs', '', None, reads_docs=True)

    def git(self, *args):
        subprocess.run(['git', '-c', 'user.name=test', '-c', 'user.email=test@example.com', *args],
                       cwd=self.root, check=True, capture_output=True)

    def prints(self):
        return validate.fingerprint(self.code), validate.fingerprint(self.docs)

    def test_staging_and_committing_do_not_change_it(self):
        before = self.prints()
        self.assertIsNotNone(before[0])
        self.git('add', '.')
        self.assertEqual(self.prints(), before)
        self.git('commit', '-q', '-m', 'first')
        self.assertEqual(self.prints(), before)

    def test_only_the_files_a_check_covers_change_it(self):
        code, docs = self.prints()
        (self.root / 'plans/status.md').write_text('second')
        self.assertEqual(self.prints(), (code, docs))
        (self.root / 'README.md').write_text('second')
        self.assertEqual(validate.fingerprint(self.code), code)
        self.assertNotEqual(validate.fingerprint(self.docs), docs)
        (self.root / 'a.cpp').write_text('second')
        self.assertNotEqual(validate.fingerprint(self.code), code)

    def test_a_new_or_deleted_file_changes_it(self):
        self.git('add', '.')
        self.git('commit', '-q', '-m', 'first')
        code = validate.fingerprint(self.code)
        (self.root / 'b.cpp').write_text('new')
        added = validate.fingerprint(self.code)
        self.assertNotEqual(added, code)
        (self.root / 'b.cpp').unlink()
        (self.root / 'a.cpp').unlink()
        self.assertNotIn(validate.fingerprint(self.code), (code, added))


class RunTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.out = Path(folder.name)
        patch = mock.patch.object(validate, 'OUT', self.out)
        patch.start()
        self.addCleanup(patch.stop)
        self.build = self.out / 'build'
        self.build.mkdir()
        (self.build / 'old.obj').write_text('compiled earlier')
        self.check = validate.Check('demo', '', None, build_dir=self.build)

    def run_python(self, code, previous):
        step = validate.Step('compile', [sys.executable, '-c', code], cwd=self.out, warning_free=True)
        return validate.run_check(self.check, [step], previous, verbose=False)

    # A stand-in compiler that crashes while `flag` does not exist yet.
    CRASH = ("import pathlib, sys; flag = pathlib.Path('crashed')\n"
             "if {always} or not flag.exists():\n"
             "    flag.touch(); print('x.c:7:1: internal compiler error: Segmentation fault'); sys.exit(1)\n"
             "print('linked')")

    def test_a_compiler_crash_is_retried_once_and_reported(self):
        self.assertTrue(self.run_python(self.CRASH.format(always=False), {}))
        log = (self.out / 'demo.log').read_text()
        self.assertIn('running this step once more', log)
        self.assertIn('linked', log)

    def test_a_compiler_crash_that_repeats_fails(self):
        with self.assertRaises(validate.Failed) as caught:
            self.run_python(self.CRASH.format(always=True), {})
        crashes = [line for line in (self.out / 'demo.log').read_text().splitlines()
                   if line.startswith('x.c:7:1: internal compiler error')]
        self.assertEqual(len(crashes), 2)
        self.assertFalse(caught.exception.warned)

    def test_a_run_without_a_crash_reports_no_retry(self):
        self.assertFalse(self.run_python("print('ok')", {}))

    def test_a_warning_fails_the_step_and_marks_the_run(self):
        with self.assertRaises(validate.Failed) as caught:
            self.run_python("print('a.c:1:2: warning: unused variable')", {})
        self.assertTrue(caught.exception.warned)
        self.assertIn('a.c:1:2', caught.exception.detail)
        self.assertIn('a.c:1:2', (self.out / 'demo.log').read_text())

    def test_the_run_after_a_warning_starts_from_an_empty_build_dir(self):
        self.run_python("print('ok')", {'result': 'fail', 'warned': True})
        self.assertFalse(self.build.exists())

    def test_other_runs_keep_the_build_dir(self):
        self.run_python("print('ok')", {'result': 'fail', 'warned': False})
        self.assertTrue((self.build / 'old.obj').exists())

    def test_a_failing_command_reports_the_end_of_its_output(self):
        with self.assertRaises(validate.Failed) as caught:
            self.run_python("import sys; print('boom'); sys.exit(3)", {})
        self.assertFalse(caught.exception.warned)
        self.assertIn('exit 3', str(caught.exception))
        self.assertIn('boom', caught.exception.detail)


if __name__ == '__main__':
    unittest.main()
