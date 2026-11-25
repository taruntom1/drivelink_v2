"""Hardware-free checks of bench flashing isolation and fail-closed guards."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench_fw
import validate


class BenchTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.firmware = self.root / 'firmware'
        self.firmware.mkdir()
        self.normal_config = self.firmware / 'sdkconfig'
        self.normal_config.write_text('# CONFIG_CORE_BENCH_CRASH is not set\n')
        self.calls = []
        self.enabled = False
        self.xip = False
        self.debug = False
        for name, value in [('FIRMWARE', self.firmware), ('OUT', self.root / 'logs')]:
            patch = mock.patch.object(bench_fw, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    def run_step(self, name, step, log, verbose):
        self.calls.append(step)
        if step.title == 'build':
            build = step.argv[step.argv.index('-B') + 1]
            header = self.firmware / build / 'config' / 'sdkconfig.h'
            header.parent.mkdir(parents=True, exist_ok=True)
            content = '#define CONFIG_CORE_BENCH_CRASH 1\n' if self.enabled else '/* #undef CONFIG_CORE_BENCH_CRASH */\n'
            content += ''.join(f'#define CONFIG_{x} 1\n' for x in (
                'PARTITION_TABLE_CUSTOM', 'BOOTLOADER_APP_ROLLBACK_ENABLE', 'ESPTOOLPY_FLASHSIZE_16MB'))
            content += '#define CONFIG_PARTITION_TABLE_CUSTOM_FILENAME "partitions.csv"\n'
            content += ('#define CONFIG_HEAP_POISONING_COMPREHENSIVE 1\n' if self.debug
                        else '#define CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK 1\n')
            if self.xip:
                content += ''.join(f'#define CONFIG_{x} 1\n' for x in (
                    'SPIRAM_XIP_FROM_PSRAM', 'SPIRAM_FETCH_INSTRUCTIONS', 'SPIRAM_RODATA'))
            header.write_text(content)

    def run_main(self, *args):
        with mock.patch.object(validate, 'idf_env', return_value={'IDF_PATH': 'idf'}), \
                mock.patch.object(validate, 'run_step', side_effect=self.run_step), \
                contextlib.redirect_stdout(io.StringIO()):
            return bench_fw.main(args)

    def test_bench_uses_only_isolated_config_and_build(self):
        self.enabled = True
        before = self.normal_config.read_bytes()
        self.assertEqual(self.run_main('bench'), 0)
        self.assertEqual([s.title for s in self.calls], ['build', 'flash'])
        for step in self.calls:
            self.assertEqual(step.cwd, self.firmware)
            self.assertIn('SDKCONFIG=build_bench/sdkconfig', step.argv)
            self.assertIn('SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench', step.argv)
        self.assertEqual(self.normal_config.read_bytes(), before)
        self.assertIn('COM3', self.calls[-1].argv)

    def test_normal_reconfigures_before_check_and_flash(self):
        self.assertEqual(self.run_main('normal', '--port', 'COM9'), 0)
        self.assertEqual([s.title for s in self.calls], ['build', 'flash'])
        self.assertEqual(self.calls[0].argv[-2:], ['reconfigure', 'build'])
        self.assertIn('SDKCONFIG=sdkconfig', self.calls[0].argv)
        self.assertIn('COM9', self.calls[-1].argv)

    def test_normal_never_flashes_bench_enabled_firmware(self):
        self.enabled = True
        self.assertEqual(self.run_main('normal'), 1)
        self.assertEqual([s.title for s in self.calls], ['build'])

    def test_bench_never_flashes_without_bench_enabled(self):
        self.assertEqual(self.run_main('bench'), 1)
        self.assertEqual([s.title for s in self.calls], ['build'])

    def test_missing_header_fails_before_flash(self):
        self.run_step = lambda *args, **kwargs: None
        self.assertEqual(self.run_main('normal'), 1)

    def test_xip_is_isolated_and_normal_rejects_xip(self):
        self.xip = True
        before = self.normal_config.read_bytes()
        self.assertEqual(self.run_main('xip'), 0)
        self.assertIn('SDKCONFIG=build_xip/sdkconfig', self.calls[0].argv)
        self.assertEqual(self.normal_config.read_bytes(), before)
        self.calls.clear()
        self.assertEqual(self.run_main('normal'), 1)
        self.assertEqual([s.title for s in self.calls], ['build'])

    def test_xip_refuses_missing_xip_or_bench_enabled(self):
        self.assertEqual(self.run_main('xip'), 1)
        self.xip = self.enabled = True
        self.assertEqual(self.run_main('xip'), 1)

    def test_debug_is_isolated_and_other_modes_reject_it(self):
        self.debug = True
        before = self.normal_config.read_bytes()
        self.assertEqual(self.run_main('debug'), 0)
        self.assertEqual([s.title for s in self.calls], ['build', 'flash'])
        for step in self.calls:
            self.assertIn('SDKCONFIG=build_debug/sdkconfig', step.argv)
            self.assertIn('SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug', step.argv)
        self.assertEqual(self.normal_config.read_bytes(), before)
        self.calls.clear()
        self.assertEqual(self.run_main('normal'), 1)
        self.assertEqual([s.title for s in self.calls], ['build'])

    def test_debug_refuses_missing_debug_or_bench_enabled(self):
        self.assertEqual(self.run_main('debug'), 1)
        self.debug = self.enabled = True
        self.assertEqual(self.run_main('debug'), 1)
        self.assertNotIn('flash', [s.title for s in self.calls])

    def test_build_failure_does_not_flash(self):
        def fail(name, step, log, verbose):
            self.calls.append(step)
            raise validate.Failed('build failed')
        self.run_step = fail
        self.assertEqual(self.run_main('normal'), 1)
        self.assertEqual([s.title for s in self.calls], ['build'])

    def test_ota_candidate_is_isolated_and_never_flashes(self):
        before = self.normal_config.read_bytes()
        self.assertEqual(self.run_main('ota-image'), 0)
        self.assertEqual([s.title for s in self.calls], ['build'])
        self.assertIn('PROJECT_VER=f9-candidate', self.calls[0].argv)
        self.assertIn('SDKCONFIG=build_ota/sdkconfig', self.calls[0].argv)
        self.assertEqual(self.normal_config.read_bytes(), before)

    def test_rollback_guard_fails_before_flash(self):
        original = self.run_step
        def missing_rollback(*args, **kwargs):
            original(*args, **kwargs)
            header = self.firmware / 'build' / 'config' / 'sdkconfig.h'
            header.write_text(header.read_text().replace('#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1', ''))
        self.run_step = missing_rollback
        self.assertEqual(self.run_main('normal'), 1)
        self.assertEqual([s.title for s in self.calls], ['build'])

    def test_dry_runs_and_help_never_export_idf_or_run_commands(self):
        with mock.patch.object(validate, 'idf_env') as export, \
                mock.patch.object(validate, 'run_step') as run, \
                contextlib.redirect_stdout(io.StringIO()):
            for mode in ('bench', 'normal', 'xip', 'debug', 'ota-image'):
                self.assertEqual(bench_fw.main([mode, '--dry-run']), 0)
            with self.assertRaises(SystemExit) as caught:
                bench_fw.main(['--help'])
            self.assertEqual(caught.exception.code, 0)
        export.assert_not_called()
        run.assert_not_called()
        self.assertFalse(bench_fw.OUT.exists())


if __name__ == '__main__':
    unittest.main()
