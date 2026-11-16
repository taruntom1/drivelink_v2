"""Tests for the pure parts of tools/validate.py."""
from pathlib import Path
import io
import json
import ctypes
import os
import signal
import subprocess
import sys
import tempfile
import time
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


class HostWindowsToolchainTest(unittest.TestCase):
    def plan(self, version):
        result = subprocess.CompletedProcess([], 0, stdout=version + '\n', stderr='')
        with mock.patch.object(validate.Path, 'is_dir', return_value=True), \
                mock.patch.object(validate.subprocess, 'run', return_value=result) as run, \
                mock.patch.dict(validate.os.environ, {'PATH': 'C:/Strawberry/c/bin'}):
            steps = validate.plan_host_win('2')
            self.assertEqual(run.call_args.args[0][0], str(validate.QT_DIRS[0] / 'g++.exe'))
            return steps

    def test_selected_qt_compiler_and_runtime_precede_runner_tools(self):
        steps = self.plan('13.1.0')
        path = steps[0].env['PATH'].split(';')
        self.assertEqual(path[:3], list(map(str, validate.QT_DIRS)))
        self.assertEqual(steps[0].argv, ['cmake', '--preset', 'windows-mingw'])
        self.assertEqual(steps[-1].title, 'installed packages')

    def test_other_mingw_versions_are_refused(self):
        for version in ('12.2.0', '15.1.0', ''):
            with self.subTest(version=version), self.assertRaises(validate.Unavailable):
                self.plan(version)

    def test_a_missing_or_broken_compiler_reports_a_missing_prerequisite(self):
        for error in (FileNotFoundError('missing compiler'), subprocess.CalledProcessError(1, 'g++')):
            with self.subTest(error=error), \
                    mock.patch.object(validate.Path, 'is_dir', return_value=True), \
                    mock.patch.object(validate.subprocess, 'run', side_effect=error), \
                    self.assertRaises(validate.Unavailable):
                validate.plan_host_win('2')


class NativeToolchainTest(unittest.TestCase):
    def windows_plan(self, missing=None):
        def exists(path):
            return path.name != missing

        with mock.patch.object(validate, 'WINDOWS', True), \
                mock.patch.object(validate, 'need_yaml'), \
                mock.patch.object(validate.Path, 'is_file', exists), \
                mock.patch.dict(validate.os.environ, {'PATH': 'C:/Strawberry/c/bin'}):
            return validate.plan_native('2')

    def test_windows_ignores_strawberry_and_keeps_ucrt_runtime_first(self):
        steps = self.windows_plan()
        configure = steps[0]
        self.assertEqual(configure.argv[0], (validate.QT_DIRS[1] / 'cmake.exe').as_posix())
        self.assertIn('-DCMAKE_C_COMPILER=' + (validate.UCRT_BIN / 'gcc.exe').as_posix(),
                      configure.argv)
        self.assertIn('-DCMAKE_CXX_COMPILER=' + (validate.UCRT_BIN / 'g++.exe').as_posix(),
                      configure.argv)
        self.assertIn('-DCMAKE_MAKE_PROGRAM=' + (validate.QT_DIRS[2] / 'ninja.exe').as_posix(),
                      configure.argv)
        build = configure.argv[configure.argv.index('-B') + 1]
        self.assertEqual(build, 'firmware/test/native/build/ucrt64')
        for step in steps:
            self.assertIn(build, step.argv)
            self.assertEqual(step.env['PATH'].split(';')[0], str(validate.UCRT_BIN))
        self.assertEqual(steps[2].argv[0], (validate.QT_DIRS[1] / 'ctest.exe').as_posix())

    def test_missing_windows_tool_never_falls_back_to_path(self):
        for tool in ('gcc.exe', 'g++.exe', 'cmake.exe', 'ctest.exe', 'ninja.exe'):
            with self.subTest(tool=tool):
                with self.assertRaises(validate.Unavailable) as error:
                    self.windows_plan(missing=tool)
                self.assertIn(tool, str(error.exception))

    def test_linux_keeps_native_tools_and_build_directory(self):
        with mock.patch.object(validate, 'WINDOWS', False), \
                mock.patch.object(validate, 'need_yaml'), \
                mock.patch.object(validate, 'need') as need:
            steps = validate.plan_native('2')
        need.assert_called_once_with('cmake', 'ctest', 'ninja', 'gcc', 'g++')
        self.assertIn('firmware/test/native/build', steps[0].argv)
        self.assertTrue(all(step.env is None for step in steps))


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

    def test_exit_does_not_wait_for_a_descendant_holding_stdout(self):
        code = ("import subprocess, sys; "
                "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(20)']); "
                "print('CHILD', child.pid, flush=True); print('failed', end='', flush=True); sys.exit(3)")
        started = time.monotonic()
        try:
            with self.assertRaises(validate.Failed) as caught:
                self.run_python(code, {})
            self.assertIn('failed', caught.exception.detail)
            self.assertLess(time.monotonic() - started, 5)
        finally:
            for line in (self.out / 'demo.log').read_text().splitlines():
                if line.startswith('CHILD '):
                    child_pid = int(line.split()[1])
                    if os.name == 'nt':
                        # TerminateProcess is asynchronous. Retain a waitable
                        # handle until the child releases its working directory.
                        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                        kernel.OpenProcess.restype = ctypes.c_void_p
                        kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
                        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint]
                        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                        child = kernel.OpenProcess(0x100001, False, child_pid)
                        if not child:
                            if ctypes.get_last_error() == 87:  # already exited
                                continue
                            raise ctypes.WinError(ctypes.get_last_error())
                        try:
                            if not kernel.TerminateProcess(child, 1):
                                error = ctypes.get_last_error()
                                if kernel.WaitForSingleObject(child, 0) != 0:
                                    raise ctypes.WinError(error)
                            self.assertEqual(kernel.WaitForSingleObject(child, 5000), 0)
                        finally:
                            kernel.CloseHandle(child)
                        continue
                    try:
                        os.kill(child_pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass


class ReportTest(unittest.TestCase):
    """Exercise reports through main without real builds or hardware."""

    class Output(io.StringIO):
        def reconfigure(self, **kwargs):
            pass

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.out = Path(folder.name)
        self.source = {'revision': 'a' * 40, 'dirty': False}
        self.checks = [validate.Check(name, '', mock.Mock(return_value=[]))
                       for name in ('gen', 'native')]
        patches = {
            'OUT': self.out, 'WINDOWS': False, 'GROUPS': False,
            'CHECKS': self.checks, 'revision': mock.Mock(return_value='aaaaaaa'),
            'source_snapshot': mock.Mock(side_effect=lambda: dict(self.source)),
            'fingerprint': mock.Mock(return_value='input-hash'),
            'run_check': mock.Mock(side_effect=self.execute),
        }
        for name, value in patches.items():
            patch = mock.patch.object(validate, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    def execute(self, check, steps, previous, verbose):
        (self.out / f'{check.name}.log').write_text('check output')
        return False

    def run_main(self, *args):
        with mock.patch.object(sys, 'stdout', self.Output()) as output:
            code = validate.main(list(args) or ['full'])
        self.output = output.getvalue()
        report = json.loads((self.out / 'summary.json').read_text())
        return code, report

    def test_full_pass_has_sources_timings_logs_and_persisted_execution(self):
        code, report = self.run_main()
        self.assertEqual(code, 0)
        self.assertEqual(report['schema_version'], 1)
        self.assertEqual(report['source'], self.source)
        self.assertEqual(report['status'], 'passed')
        self.assertTrue(report['full_gate_passed'])
        self.assertEqual(report['omitted_checks'], [])
        self.assertEqual(report['hardware_status'], 'not_run')
        self.assertLessEqual(report['started_at'], report['finished_at'])
        state = json.loads((self.out / 'state.json').read_text())
        for record in report['checks']:
            self.assertEqual(record['status'], 'passed')
            self.assertEqual(record['input_fingerprint'], 'input-hash')
            self.assertGreaterEqual(record['duration_seconds'], 0)
            self.assertTrue(Path(record['log_path']).is_absolute())
            self.assertTrue(Path(record['log_path']).is_file())
            self.assertEqual(record['execution']['source'], self.source)
            self.assertEqual(state[record['name']]['execution'], record['execution'])
            self.assertFalse(record['compiler_crash_retried'])
        self.assertIn('Result: PASS', self.output)
        summary_output = ''.join(line for line in self.output.splitlines(keepends=True)
                                 if not line.rstrip().endswith('running...'))
        self.assertEqual((self.out / 'summary.txt').read_text(), summary_output)

    def test_reuse_keeps_original_provenance_separate_from_current_revision(self):
        _, original = self.run_main()
        self.source = {'revision': 'b' * 40, 'dirty': True}
        validate.run_check.reset_mock()
        _, reused = self.run_main()
        self.assertEqual(reused['source'], self.source)
        self.assertTrue(reused['full_gate_passed'])
        validate.run_check.assert_not_called()
        for before, after in zip(original['checks'], reused['checks']):
            self.assertEqual(after['status'], 'reused')
            self.assertEqual(after['execution'], before['execution'])
            self.assertIsNone(after['duration_seconds'])
            self.assertEqual(after['log_path'], before['log_path'])
        self.assertIn('pass (unchanged)', self.output)

    def test_legacy_cache_reuses_with_unknown_provenance_and_no_stale_log_claim(self):
        (self.out / 'state.json').write_text(json.dumps({
            'gen': {'result': 'pass', 'fingerprint': 'input-hash'}}))
        code, report = self.run_main('--only', 'gen')
        self.assertEqual(code, 0)
        self.assertFalse(report['full_gate_passed'])
        self.assertEqual(report['omitted_checks'], ['native'])
        record = report['checks'][0]
        self.assertEqual(record['status'], 'reused')
        self.assertIsNone(record['execution'])
        self.assertIsNone(record['log_path'])
        self.assertEqual(report['checks'][1]['status'], 'not_selected')
        validate.run_check.assert_not_called()

    def test_fail_fast_distinguishes_failure_from_unexecuted_checks(self):
        def fail(check, *args):
            (self.out / f'{check.name}.log').write_text('boom')
            raise validate.Failed('compile failed (exit 3)', 'boom', warned=True)
        validate.run_check.side_effect = fail
        code, report = self.run_main('full', '--fail-fast')
        self.assertEqual(code, 1)
        self.assertEqual(report['status'], 'failed')
        self.assertFalse(report['full_gate_passed'])
        failure, skipped = report['checks']
        self.assertEqual(failure['status'], 'failed')
        self.assertEqual(failure['failure_reason'], 'compile failed (exit 3)')
        self.assertEqual(failure['diagnostic_excerpt'], 'boom')
        self.assertIsNotNone(failure['execution'])
        self.assertTrue(Path(failure['log_path']).is_file())
        self.assertEqual(skipped['status'], 'skipped_after_failure')
        self.assertIsNone(skipped['log_path'])
        self.assertIsNone(skipped['execution'])
        self.assertEqual(validate.run_check.call_count, 1)
        state = json.loads((self.out / 'state.json').read_text())
        self.assertTrue(state['gen']['warned'])
        self.assertNotIn('native', state)

    def test_unavailable_does_not_claim_an_old_log_or_prevent_other_checks(self):
        self.checks[0].plan.side_effect = validate.Unavailable('PyYAML missing')
        (self.out / 'gen.log').write_text('old success')
        code, report = self.run_main()
        self.assertEqual(code, 1)
        self.assertFalse(report['full_gate_passed'])
        record = report['checks'][0]
        self.assertEqual(record['status'], 'unavailable')
        self.assertEqual(record['failure_reason'], 'PyYAML missing')
        self.assertIsNone(record['execution'])
        self.assertIsNone(record['log_path'])
        self.assertIsNone(record['duration_seconds'])
        self.assertEqual(report['checks'][1]['status'], 'passed')

    def test_skip_cannot_claim_full_gate(self):
        code, report = self.run_main('full', '--skip', 'native')
        self.assertEqual(code, 0)
        self.assertFalse(report['full_gate_passed'])
        self.assertEqual(report['request']['skip'], ['native'])
        self.assertEqual(report['checks'][1]['status'], 'not_selected')

    def test_force_refreshes_execution_and_reports_compiler_retry(self):
        self.run_main()
        self.source['revision'] = 'b' * 40
        validate.run_check.side_effect = None
        validate.run_check.return_value = True
        _, report = self.run_main('full', '--force')
        for record in report['checks']:
            self.assertEqual(record['status'], 'passed')
            self.assertEqual(record['execution']['source'], self.source)
            self.assertTrue(record['compiler_crash_retried'])
        _, reused = self.run_main()
        self.assertTrue(reused['checks'][0]['compiler_crash_retried'])

    def test_unknown_fingerprints_are_null_and_never_reused(self):
        validate.fingerprint.return_value = None
        self.run_main()
        validate.run_check.reset_mock()
        _, report = self.run_main()
        self.assertEqual(validate.run_check.call_count, 2)
        self.assertIsNone(report['checks'][0]['input_fingerprint'])
        self.assertEqual(report['checks'][0]['status'], 'passed')

    def test_failed_atomic_publish_keeps_previous_report_and_cleans_temporary(self):
        self.run_main()
        previous = (self.out / 'summary.json').read_bytes()
        with mock.patch.object(validate.os, 'replace', side_effect=OSError('cannot replace')):
            with self.assertRaises(OSError):
                validate.write_report({'new': 'report'})
        self.assertEqual((self.out / 'summary.json').read_bytes(), previous)
        self.assertEqual(list(self.out.glob('summary-*.tmp')), [])


if __name__ == '__main__':
    unittest.main()
