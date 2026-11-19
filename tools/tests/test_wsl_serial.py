"""Hardware-free checks of the WSL serial hand-off: parsing, selection, guards, `run` always detaching."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import wsl_serial


def raw(busid, vid_pid='1a86:55d3', bound=True, client=None, description='USB-Enhanced-SERIAL CH343 (COM5)'):
    vid, pid = vid_pid.upper().split(':')
    return {'BusId': busid, 'ClientIPAddress': client, 'Description': description,
            'InstanceId': f'USB\\VID_{vid}&PID_{pid}\\5B5E080134', 'IsForced': False,
            'PersistedGuid': 'f10253bd-e38e-4df3-a8a4-f2e9118794c7' if bound else None,
            'StubInstanceId': None}


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class World:
    """usbipd and wsl.exe, as far as the tool can see them."""

    def __init__(self, *devices):
        self.devices = list(devices)
        self.calls = []
        self.tty_appears = True      # attach makes /dev/ttyACM0 appear
        self.detach_works = True
        self.attach_code = 0
        self.ttys = []
        self.attach_hook = None

    def device(self, busid):
        return next(d for d in self.devices if d['BusId'] == busid)

    def capture(self, argv, timeout=30):
        self.calls.append(argv)
        name = Path(argv[0]).stem
        if name == 'usbipd':
            verb = argv[1]
            if verb == 'state':
                return 0, '﻿' + json.dumps({'Devices': self.devices}), ''
            busid = argv[argv.index('--busid') + 1]
            if verb == 'attach':
                if self.attach_code:
                    return self.attach_code, '', 'attach exploded'
                self.device(busid)['ClientIPAddress'] = '172.20.0.1'
                if self.tty_appears:
                    self.ttys = ['/dev/ttyACM0']
                if self.attach_hook:
                    self.attach_hook()
                return 0, '', ''
            if verb == 'detach':
                if self.detach_works:
                    self.device(busid)['ClientIPAddress'] = None
                    self.ttys = []
                return 0, '', ''
        if name == 'wsl':
            return 0, ''.join(t + '\n' for t in self.ttys), ''
        raise AssertionError(f'unexpected command {argv}')

    def verbs(self):
        return [c[1] for c in self.calls if Path(c[0]).stem == 'usbipd']


class ToolTest(unittest.TestCase):
    def setUp(self):
        self.world = World(raw('1-6'))
        patches = [mock.patch.object(wsl_serial, 'capture', side_effect=lambda *a, **k: self.world.capture(*a, **k)),
                   mock.patch.object(wsl_serial, 'time', FakeClock())]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = wsl_serial.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def run_child(self, code=0, exc=None):
        """Patch the CMD launcher of `run`; records the call."""
        def call(command, env=None):
            self.child = (command, env)
            if exc:
                raise exc
            return code
        return mock.patch.object(wsl_serial.subprocess, 'call', side_effect=call)


class ParseAndSelectTest(unittest.TestCase):
    def test_states(self):
        text = json.dumps({'Devices': [
            raw(None, description='ghost'),
            raw('1-1', bound=False),
            raw('1-2'),
            raw('1-3', client='172.20.0.1'),
            raw('1-4', vid_pid='303A:1001')]})
        devices = wsl_serial.parse_state('banner line\n' + text)
        self.assertEqual([d.state for d in devices], ['not-connected', 'not-shared', 'shared', 'attached', 'shared'])
        self.assertEqual(devices[0].vid_pid, '1a86:55d3')
        self.assertEqual(devices[4].vid_pid, '303a:1001')

    def test_missing_fields_and_bad_json(self):
        self.assertEqual(wsl_serial.parse_state('{}'), [])
        self.assertEqual(wsl_serial.parse_state('{"Devices":[{"BusId":"1-1"}]}')[0].vid_pid, '')
        with self.assertRaises(ValueError):
            wsl_serial.parse_state('not json')

    def test_selection(self):
        devices = wsl_serial.parse_state(json.dumps({'Devices': [
            raw(None), raw(None), raw('1-2'), raw('1-5', vid_pid='303a:1001')]}))
        self.assertEqual(wsl_serial.select(devices, '1a86:55d3').busid, '1-2')  # unplugged ones do not count
        self.assertEqual(wsl_serial.select(devices, '1a86:55d3', '1-5').busid, '1-5')
        with self.assertRaises(wsl_serial.ToolError) as caught:
            wsl_serial.select(devices, '1a86:55d3', '9-9')
        self.assertTrue(caught.exception.missing)
        with self.assertRaises(wsl_serial.ToolError) as caught:
            wsl_serial.select(devices, '0000:0000')
        self.assertTrue(caught.exception.missing)

    def test_ambiguous_and_prefer_attached(self):
        devices = wsl_serial.parse_state(json.dumps({'Devices': [raw('1-2'), raw('1-3', client='x')]}))
        with self.assertRaises(wsl_serial.ToolError) as caught:
            wsl_serial.select(devices, '1a86:55d3')
        self.assertFalse(caught.exception.missing)
        self.assertIn('--busid', str(caught.exception))
        self.assertEqual(wsl_serial.select(devices, '1a86:55d3', prefer_attached=True).busid, '1-3')
        self.assertEqual(wsl_serial.select(devices, '1a86:55d3', '1-2').busid, '1-2')


class StatusTest(ToolTest):
    def test_each_state_text(self):
        for device, label in [(raw('1-6', bound=False), 'not shared'), (raw('1-6'), 'shared, on Windows'),
                              (raw('1-6', client='172.20.0.1'), 'attached to WSL')]:
            self.world = World(device)
            self.world.ttys = ['/dev/ttyACM0']
            code, out, _ = self.run_main('status')
            self.assertEqual(code, 0)
            self.assertIn(label, out)
        self.assertIn('/dev/ttyACM0', out)

    def test_wsl_asked_only_when_attached(self):
        self.run_main('status')
        self.assertEqual(self.world.verbs(), ['state'])
        self.assertEqual(len(self.world.calls), 1)

    def test_json_and_none_found(self):
        self.world = World(raw('1-6', client='172.20.0.1'), raw(None, description='ghost'))
        self.world.ttys = ['/dev/ttyUSB0']
        code, out, _ = self.run_main('status', '--json')
        data = json.loads(out)
        self.assertEqual(code, 0)
        self.assertTrue(data['found'])
        self.assertEqual(data['wsl_ttys'], ['/dev/ttyUSB0'])
        self.assertEqual([d['state'] for d in data['devices']], ['attached', 'not-connected'])
        self.world = World(raw(None), raw('1-1', vid_pid='303a:1001'))
        code, out, _ = self.run_main('status')
        self.assertEqual(code, 3)
        self.assertIn('not connected', out)
        self.assertEqual(self.run_main('status', '--busid', '9-9')[0], 3)


class AttachTest(ToolTest):
    def test_attach_prints_tty_and_runs_expected_command(self):
        code, out, _ = self.run_main('attach', '--distro', 'Ubuntu-26.04')
        self.assertEqual(code, 0)
        self.assertIn('/dev/ttyACM0', out)
        attach = next(c for c in self.world.calls if len(c) > 1 and c[1] == 'attach')
        self.assertEqual(attach[1:], ['attach', '--busid', '1-6', '--wsl', 'Ubuntu-26.04'])
        self.assertIn('-d', next(c for c in self.world.calls if Path(c[0]).stem == 'wsl'))

    def test_default_distro_is_bare_wsl_flag(self):
        self.run_main('attach')
        attach = next(c for c in self.world.calls if len(c) > 1 and c[1] == 'attach')
        self.assertEqual(attach[1:], ['attach', '--busid', '1-6', '--wsl'])

    def test_refuses_missing_ambiguous_and_unshared(self):
        self.world = World(raw(None))
        code, _, err = self.run_main('attach')
        self.assertEqual(code, 2)
        self.assertIn('plug', err)
        self.world = World(raw('1-6'), raw('1-7'))
        code, _, err = self.run_main('attach')
        self.assertEqual(code, 2)
        self.assertIn('--busid', err)
        self.assertEqual(self.run_main('attach', '--busid', '1-7')[0], 0)
        self.world = World(raw('1-6', bound=False))
        code, _, err = self.run_main('attach')
        self.assertEqual(code, 2)
        self.assertIn('usbipd bind --busid 1-6', err)
        self.assertEqual(self.world.verbs(), ['state'])

    def test_already_attached_is_success_without_attach(self):
        self.world = World(raw('1-6', client='172.20.0.1'))
        self.world.ttys = ['/dev/ttyACM1']
        code, out, _ = self.run_main('attach')
        self.assertEqual(code, 0)
        self.assertIn('already attached', out)
        self.assertNotIn('attach', self.world.verbs())

    def test_attach_failure_and_missing_tty(self):
        self.world.attach_code = 1
        code, _, err = self.run_main('attach')
        self.assertEqual(code, 1)
        self.assertIn('attach exploded', err)
        self.world = World(raw('1-6'))
        self.world.tty_appears = False
        code, _, err = self.run_main('attach', '--timeout', '2')
        self.assertEqual(code, 1)
        self.assertIn('no /dev/ttyACM*', err)

    def test_preexisting_tty_is_not_mistaken_for_the_new_one(self):
        self.world.ttys = ['/dev/ttyUSB0']
        self.world.tty_appears = False
        self.assertEqual(self.run_main('attach', '--timeout', '1')[0], 1)

    def test_usbipd_missing(self):
        with mock.patch.object(wsl_serial, 'capture', side_effect=FileNotFoundError('nope')):
            code, _, err = self.run_main('attach')
        self.assertEqual(code, 2)
        self.assertIn('usbipd', err)


class DryRunTest(ToolTest):
    def test_dry_run_prints_and_changes_nothing(self):
        for mode in ('attach', 'detach'):
            code, out, _ = self.run_main(mode, '--dry-run')
            self.assertEqual(code, 0)
        self.assertEqual(self.world.verbs(), ['state', 'state'])
        self.assertIsNone(self.world.device('1-6')['ClientIPAddress'])
        self.assertEqual(self.world.calls[0][1], 'state')

    def test_dry_run_attach_text(self):
        _, out, _ = self.run_main('attach', '--dry-run')
        self.assertIn('DRY RUN: ', out)
        self.assertIn('attach --busid 1-6 --wsl', out)

    def test_dry_run_still_refuses_unshared(self):
        self.world = World(raw('1-6', bound=False))
        code, _, err = self.run_main('attach', '--dry-run')
        self.assertEqual(code, 2)
        self.assertIn('usbipd bind', err)

    def test_usbipd_missing_assumes_device_only_with_busid(self):
        calls = []
        def missing(argv, timeout=30):
            calls.append(argv)
            raise FileNotFoundError('no usbipd')
        with mock.patch.object(wsl_serial, 'capture', side_effect=missing):
            code, out, _ = self.run_main('attach', '--dry-run', '--busid', '1-9')
            self.assertEqual(code, 0)
            self.assertIn('assuming 1-9', out)
            self.assertIn('attach --busid 1-9', out)
            code, out, _ = self.run_main('detach', '--dry-run', '--busid', '1-9')
            self.assertEqual((code, 'detach --busid 1-9' in out), (0, True))
            self.assertEqual(self.run_main('attach', '--dry-run')[0], 2)
            self.assertEqual(self.run_main('attach', '--busid', '1-9')[0], 2)  # not a dry run: refuse
        self.assertTrue(all(c[1] == 'state' for c in calls))

    def test_dry_run_run_never_starts_command(self):
        with self.run_child() as call:
            code, out, _ = self.run_main('run', '--dry-run', '--', 'wsl.exe', '-e', 'agent', 'x y')
        self.assertEqual(code, 0)
        call.assert_not_called()
        self.assertIn(f'{wsl_serial.ENV_TTY}=', out)
        self.assertIn('"x y"', out)
        self.assertEqual(self.world.verbs(), ['state'])


class DetachTest(ToolTest):
    def test_detach_waits_until_back_on_windows(self):
        self.world = World(raw('1-6', client='172.20.0.1'))
        code, out, _ = self.run_main('detach')
        self.assertEqual(code, 0)
        self.assertIn('back on Windows', out)
        self.assertEqual(self.world.verbs(), ['state', 'detach', 'state'])

    def test_not_attached_or_gone_is_noop_success(self):
        self.assertEqual(self.run_main('detach')[0], 0)
        self.assertNotIn('detach', self.world.verbs())
        self.world = World(raw(None))
        code, out, _ = self.run_main('detach')
        self.assertEqual(code, 0)
        self.assertIn('nothing to detach', out)

    def test_detach_picks_the_attached_one(self):
        self.world = World(raw('1-6'), raw('1-7', client='172.20.0.1'))
        self.assertEqual(self.run_main('detach')[0], 0)
        self.assertIsNone(self.world.device('1-7')['ClientIPAddress'])

    def test_detach_timeout_is_reported(self):
        self.world = World(raw('1-6', client='172.20.0.1'))
        self.world.detach_works = False
        code, _, err = self.run_main('detach', '--timeout', '2')
        self.assertEqual(code, 4)
        self.assertIn('usbipd detach --busid 1-6', err)

    def test_unreadable_state_is_not_success(self):
        self.world = World(raw('1-6', client='172.20.0.1'))
        original = self.world.capture
        def flaky(argv, timeout=30):
            if argv[1:2] == ['state'] and 'detach' in self.world.verbs():
                return 1, '', 'boom'
            return original(argv, timeout)
        with mock.patch.object(wsl_serial, 'capture', side_effect=flaky):
            self.assertEqual(self.run_main('detach', '--timeout', '1')[0], 4)


class RunTest(ToolTest):
    def assert_detached(self):
        self.assertIsNone(self.world.device('1-6')['ClientIPAddress'])
        self.assertEqual(self.world.verbs()[-2:], ['detach', 'state'])

    def test_success_passes_tty_and_detaches(self):
        with mock.patch.dict(wsl_serial.os.environ, {'WSLENV': 'FOO/p'}), self.run_child(0):
            code, out, _ = self.run_main('run', '--', 'wsl.exe', '-e', 'agent')
        self.assertEqual(code, 0)
        command, env = self.child
        self.assertEqual(command, ['wsl.exe', '-e', 'agent'])
        self.assertEqual(env['DRIVELINK_WSL_TTY'], '/dev/ttyACM0')
        self.assertEqual(env['WSLENV'], 'FOO/p:DRIVELINK_WSL_TTY')
        self.assert_detached()

    def test_cmd_failure_returns_its_code_and_detaches(self):
        with self.run_child(7):
            self.assertEqual(self.run_main('run', 'tool')[0], 7)
        self.assert_detached()

    def test_cmd_cannot_start(self):
        with self.run_child(exc=FileNotFoundError('tool')):
            code, _, err = self.run_main('run', 'tool')
        self.assertEqual(code, 127)
        self.assert_detached()

    def test_keyboard_interrupt_detaches(self):
        with self.run_child(exc=KeyboardInterrupt()):
            code, _, err = self.run_main('run', 'tool')
        self.assertEqual(code, 130)
        self.assert_detached()

    def test_interrupt_while_attach_runs_still_detaches(self):
        def interrupt():
            raise KeyboardInterrupt
        self.world.attach_hook = interrupt
        with self.run_child() as call:
            code, _, _ = self.run_main('run', 'tool')
        self.assertEqual(code, 130)
        call.assert_not_called()
        self.assert_detached()

    def test_interrupt_during_detach_is_retried(self):
        self.world = World(raw('1-6'))
        original = self.world.capture
        interrupts = [KeyboardInterrupt()]
        def flaky(argv, timeout=30):
            if argv[1:2] == ['detach'] and interrupts:
                raise interrupts.pop()
            return original(argv, timeout)
        with mock.patch.object(wsl_serial, 'capture', side_effect=flaky), self.run_child(0):
            code, _, _ = self.run_main('run', 'tool')
        self.assertEqual(code, 0)
        self.assert_detached()

    def test_attach_without_tty_detaches_and_never_starts_command(self):
        self.world.tty_appears = False
        with self.run_child() as call:
            code, _, err = self.run_main('run', '--timeout', '1', 'tool')
        self.assertEqual(code, 1)
        call.assert_not_called()
        self.assertIn('no /dev/ttyACM*', err)
        self.assert_detached()

    def test_failed_attach_command_still_detaches(self):
        self.world.attach_code = 1
        with self.run_child() as call:
            code, _, _ = self.run_main('run', 'tool')
        self.assertEqual(code, 1)
        call.assert_not_called()
        self.assertIn('detach', self.world.verbs())

    def test_detach_failure_code_wins(self):
        self.world.detach_works = False
        with self.run_child(0):
            code, _, err = self.run_main('run', '--timeout', '1', 'tool')
        self.assertEqual(code, 4)
        self.assertIn('still attached', err)
        with self.run_child(9):
            self.assertEqual(self.run_main('run', '--timeout', '1', 'tool')[0], 4)

    def test_already_attached_is_detached_at_the_end(self):
        self.world = World(raw('1-6', client='172.20.0.1'))
        self.world.ttys = ['/dev/ttyACM0']
        with self.run_child(0):
            self.assertEqual(self.run_main('run', 'tool')[0], 0)
        self.assertIsNone(self.world.device('1-6')['ClientIPAddress'])
        self.assertNotIn('attach', self.world.verbs())

    def test_refusals_attach_nothing_and_detach_nothing(self):
        for world in (World(raw(None)), World(raw('1-6', bound=False)), World(raw('1-6'), raw('1-7'))):
            self.world = world
            with self.run_child() as call:
                self.assertEqual(self.run_main('run', 'tool')[0], 2)
            call.assert_not_called()
            self.assertEqual(self.world.verbs(), ['state'])


class ArgsTest(unittest.TestCase):
    def test_run_command_with_and_without_separator(self):
        args = wsl_serial.parse_args(['run', '--busid', '1-2', '--timeout', '5', '--', 'wsl.exe', '-e', 'a', '--b'])
        self.assertEqual((args.busid, args.timeout, args.command), ('1-2', 5.0, ['wsl.exe', '-e', 'a', '--b']))
        self.assertEqual(wsl_serial.parse_args(['run', 'tool', 'x']).command, ['tool', 'x'])

    def test_run_needs_command_and_help_exits(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for argv in (['run'], ['run', '--'], []):
                with self.assertRaises(SystemExit) as caught:
                    wsl_serial.parse_args(argv)
                self.assertEqual(caught.exception.code, 2)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                wsl_serial.parse_args(['--help'])
        self.assertEqual(caught.exception.code, 0)

    def test_defaults(self):
        args = wsl_serial.parse_args(['status'])
        self.assertEqual((args.vid_pid, args.timeout, args.distro, args.dry_run), ('1a86:55d3', 10.0, None, False))


if __name__ == '__main__':
    unittest.main()
