"""Hardware-free checks of the JTAG tool: ELF matching, scripts, cleanup and the VS Code config."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import jtag
import validate


def app_desc(sha, version='0.2.0', project='drivelink_v2', magic=jtag.APP_DESC_MAGIC):
    data = bytearray(256)
    data[0:4] = magic.to_bytes(4, 'little')
    data[16:16 + len(version)] = version.encode()
    data[48:48 + len(project)] = project.encode()
    data[144:176] = bytes.fromhex(sha)
    return [int.from_bytes(data[i:i + 4], 'little') for i in range(0, 256, 4)]


class ElfTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.elf = self.root / 'firmware' / 'build_debug' / 'drivelink_v2.elf'
        self.elf.parent.mkdir(parents=True)
        self.elf.write_bytes(b'the elf')
        self.sha = hashlib.sha256(b'the elf').hexdigest()
        patch = mock.patch.object(jtag, 'worktrees', return_value=[self.root])
        patch.start()
        self.addCleanup(patch.stop)

    def args(self, *options):
        return jtag.arguments(['bt', *options])

    def test_app_desc(self):
        app = jtag.parse_app_desc(app_desc(self.sha))
        self.assertEqual(app, jtag.App('drivelink_v2', '0.2.0', self.sha))

    def test_app_desc_refuses_other_data(self):
        for words in (app_desc(self.sha, magic=1), app_desc(self.sha)[:20]):
            with self.assertRaises(jtag.ToolError):
                jtag.parse_app_desc(words)

    def test_match_is_found_in_any_build_directory(self):
        app = jtag.App('drivelink_v2', '0.2.0', self.sha)
        self.assertEqual(jtag.choose_elf(app, self.args()), self.elf)
        self.assertEqual(jtag.find_elf(self.sha[:9].upper(), jtag.candidates()), self.elf)

    def test_match_wins_over_a_given_elf(self):
        other = self.root / 'other.elf'
        other.write_bytes(b'other')
        app = jtag.App('drivelink_v2', '0.2.0', self.sha)
        self.assertEqual(jtag.choose_elf(app, self.args('--elf', str(other), '--allow-mismatch')), self.elf)

    def test_no_match_is_refused_without_the_override(self):
        app = jtag.App('drivelink_v2', '0.2.0', 'ab' * 32)
        for options in ((), ('--elf', str(self.elf)), ('--allow-mismatch',), ('--elf', 'missing.elf')):
            with self.subTest(options=options), self.assertRaises(jtag.ToolError) as caught:
                jtag.choose_elf(app, self.args(*options))
            self.assertEqual(caught.exception.code, jtag.REFUSED)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            chosen = jtag.choose_elf(app, self.args('--elf', str(self.elf), '--allow-mismatch'))
        self.assertEqual(chosen, self.elf)
        self.assertIn('WARNING', err.getvalue())

    def test_gdb_init_drops_the_build_directory_app(self):
        symbols = self.elf.parent / 'gdbinit' / 'symbols'
        self.assertNotIn('add-symbol-file', jtag.gdb_init(self.elf))
        symbols.parent.mkdir()
        symbols.write_text('add-symbol-file rom.elf\nset confirm on\nfile app.elf\n')
        lines = jtag.gdb_init(self.elf).splitlines()
        self.assertIn('add-symbol-file rom.elf', lines)
        self.assertFalse([line for line in lines if line.startswith('file ')])
        self.assertEqual(lines[-1], 'set confirm off')


class ScriptTest(unittest.TestCase):
    def test_break_dumps_each_stop(self):
        args = jtag.arguments(['break', 'core::f', '--count', '3', '--dump', 'x', '--timeout', '7'])
        commands, limit = jtag.script_for(args)
        self.assertEqual(limit, 7)
        self.assertEqual(commands[0], 'hbreak core::f')
        self.assertIn('while $hit < 3', commands)
        self.assertIn('  print x', commands)
        self.assertEqual(commands[-2:], ['end', 'delete'])

    def test_watch_is_on_the_location(self):
        commands, _ = jtag.script_for(jtag.arguments(['watch', 'a->b']))
        self.assertEqual(commands[0], 'watch -location a->b')

    def test_bad_arguments(self):
        for argv in (['break', 'f', '--count', '0'], ['break', 'f', '--timeout', '0'],
                     ['break', 'f', '--timeout', 'inf'], ['print', 'x\nshell dir'], ['print'], []):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit):
                jtag.arguments(argv)

    def test_panic_log(self):
        log = ('ELF file SHA256: 9dcbd1e32\n'
               'Backtrace: 0x42011418:0x3fcbd000 0x420115d8:0x3fcbd020 |<-CORRUPTED\n')
        self.assertEqual(jtag.panic_addresses(log), ('9dcbd1e32', [['0x42011418', '0x420115d8']]))
        self.assertEqual(jtag.panic_addresses('boot\n'), (None, []))

    def test_decode_refuses_an_unknown_elf(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(jtag, 'worktrees', return_value=[]):
            log = Path(folder) / 'log.txt'
            log.write_text('ELF file SHA256: 0123\nBacktrace: 0x42011418:0x3fcbd000\n')
            with self.assertRaises(jtag.ToolError) as caught:
                jtag.decode(jtag.arguments(['decode', str(log)]), {})
        self.assertEqual(caught.exception.code, jtag.REFUSED)

    def test_openocd_errors(self):
        self.assertEqual(jtag.openocd_error('LIBUSB_ERROR_ACCESS').code, jtag.REFUSED)
        self.assertIn('reserved', str(jtag.openocd_error("Error: couldn't bind gdb to socket on port 23333")))
        self.assertIn('no ESP32-S3', str(jtag.openocd_error('Error: esp_usb_jtag: could not find or open device!')))


class CleanupTest(unittest.TestCase):
    """The board must run again however a session ends."""

    def setUp(self):
        self.process = mock.Mock()
        for name in ('tcl', 'recover'):
            patch = mock.patch.object(jtag, name)
            setattr(self, name, patch.start())
            self.addCleanup(patch.stop)

    def test_clean_end(self):
        self.process.poll.return_value = None
        jtag.release(self.process, {})
        self.tcl.assert_called_once_with('shutdown')
        self.recover.assert_not_called()

    def test_openocd_that_does_not_end_is_killed_and_the_board_recovered(self):
        self.process.poll.return_value = None
        self.process.wait.side_effect = [jtag.subprocess.TimeoutExpired('openocd', 10), 0]
        jtag.release(self.process, {})
        self.process.kill.assert_called_once()
        self.recover.assert_called_once()

    def test_openocd_that_died_means_recovery(self):
        self.process.poll.return_value = 1
        self.process.returncode = 1
        jtag.release(self.process, {})
        self.recover.assert_called_once()

    def test_openocd_that_never_started_or_was_stopped_needs_none(self):
        self.process.poll.return_value = 1
        self.process.returncode = 1
        jtag.release(self.process, {}, started=False)
        self.process.returncode = 0
        jtag.release(self.process, {})
        self.recover.assert_not_called()

    def test_app_is_released_when_reading_fails(self):
        self.tcl.side_effect = ['', OSError(), '']
        with self.assertRaises(OSError):
            jtag.running_app()
        self.assertEqual(self.tcl.call_args.args, ('drivelink_release',))

    def test_board_without_an_app_is_an_error(self):
        self.tcl.side_effect = ['', 'no app description found', '']
        with self.assertRaises(jtag.ToolError):
            jtag.running_app()

    def test_open_session_is_refused(self):
        with mock.patch.object(jtag, 'listening', return_value=True), \
                self.assertRaises(jtag.ToolError) as caught, jtag.openocd({}):
            pass
        self.assertEqual(caught.exception.code, jtag.REFUSED)


class ConfigTest(unittest.TestCase):
    def test_openocd_config(self):
        text = jtag.CONFIG.read_text(encoding='utf-8')
        code = '\n'.join(line.split('#')[0] for line in text.splitlines())
        self.assertIn('set ESP_FLASH_SIZE 0', code)  # never writes flash
        self.assertLess(code.index('ESP_FLASH_SIZE'), code.index('source'))
        self.assertIn('bindto 127.0.0.1', code)
        self.assertIn(f'gdb port {jtag.GDB_PORT}', code)
        self.assertIn(f'tcl port {jtag.TCL_PORT}', code)
        self.assertNotIn('reset', code)             # the stock attach hook resets the chip
        self.assertIn('-event gdb-detach { drivelink_release }', code)
        self.assertNotIn('gdb-end', code)           # comes at every stop, not at the end
        self.assertIn('pre_shutdown_commands drivelink_release', code)

    def vscode(self, name):
        text = (validate.ROOT / '.vscode' / name).read_text(encoding='utf-8')
        return json.loads(re.sub(r'^\s*//.*$', '', text, flags=re.MULTILINE))

    def test_vscode_config_matches_the_tool(self):
        tasks = {task['label']: task for task in self.vscode('tasks.json')['tasks']}
        configurations = self.vscode('launch.json')['configurations']
        self.assertEqual(len(configurations), 2)
        for configuration in configurations:
            paths = {'program': jtag.SESSION_ELF, 'init': jtag.SESSION_INIT}
            self.assertEqual(configuration['miDebuggerServerAddress'], f'127.0.0.1:{jtag.GDB_PORT}')
            root = '${workspaceFolder}/'
            self.assertEqual(configuration['program'],
                             root + paths['program'].relative_to(validate.ROOT).as_posix())
            self.assertEqual(configuration['setupCommands'][0]['text'],
                             f"source {root}{paths['init'].relative_to(validate.ROOT).as_posix()}")
            for key in ('preLaunchTask', 'postDebugTask'):
                if key in configuration:
                    self.assertIn(configuration[key], tasks)
        for task in tasks.values():
            jtag.arguments(task['args'][1:])
        ends = tasks['JTAG server']['problemMatcher']['background']['endsPattern']
        self.assertTrue(re.match(ends, 'READY: GDB') and re.match(ends, 'ERROR: x'))
        begins = tasks['JTAG server']['problemMatcher']['background']['beginsPattern']
        self.assertTrue(re.match(begins, jtag.WARNING))


if __name__ == '__main__':
    unittest.main()
