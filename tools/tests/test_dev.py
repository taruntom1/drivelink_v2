"""Hardware-free checks of tools/dev.py and of the VS Code tasks that call the tools."""
import contextlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench_fw
import dev
import jtag
import release
import validate


def quiet(call, *args):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return call(*args)


class CommandTest(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.object(dev, 'run', return_value=0)
        self.run_mock = patch.start()
        self.addCleanup(patch.stop)

    def test_monitor_never_asks_for_a_reset(self):
        args = dev.arguments(['fw', 'monitor', '--port', 'COM9'])
        self.assertEqual(dev.idf_command(args), ['-p', 'COM9', 'monitor', '--no-reset'])

    def test_no_firmware_command_flashes(self):
        for action in dev.IDF_ACTIONS:
            command = dev.idf_command(dev.arguments(['fw', action]))
            self.assertFalse([word for word in command if 'flash' in word or 'erase' in word])
        with self.assertRaises(SystemExit):
            quiet(dev.arguments, ['fw', 'flash'])

    def test_cli_arguments_are_passed_on(self):
        self.assertEqual(dev.arguments(['cli', '--endpoint', 'serial://COM3', '--help']).args,
                         ['--endpoint', 'serial://COM3', '--help'])

    def test_gui_takes_one_device(self):
        with self.assertRaises(SystemExit):
            quiet(dev.arguments, ['gui', '--vdev', '--endpoint', 'serial://COM3'])

    def test_run_all_stops_at_the_first_failure(self):
        self.run_mock.side_effect = [0, 3, 0]
        steps = [validate.Step(str(n), ['x']) for n in range(3)]
        self.assertEqual(dev.run_all(steps), 3)
        self.assertEqual(self.run_mock.call_count, 2)

    def test_notices_match_the_checked_ones(self):
        self.assertEqual(dev.notices(None), 0)
        written = [' '.join(call.args[0][1:]).replace('--write', '--check') for call in self.run_mock.call_args_list]
        with mock.patch.object(validate, 'need_yaml'):
            checked = [' '.join(step.argv[1:]) for step in validate.plan_gen('1') if 'gen_notices' in step.argv[1]]
        self.assertEqual(written, checked)

    def test_guide_creates_its_environment_once(self):
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.object(dev, 'VENV_PYTHON', Path(folder) / 'python'):
            dev.guide(dev.arguments(['guide', 'build']))
            self.assertEqual([call.args[0][1:3] for call in self.run_mock.call_args_list],
                             [['-m', 'venv'], ['-m', 'pip'], ['tools/guide.py', 'build']])
            self.run_mock.reset_mock()
            dev.VENV_PYTHON.touch()
            dev.guide(dev.arguments(['guide', 'serve']))
            self.assertEqual([call.args[0][1:] for call in self.run_mock.call_args_list], [['tools/guide.py', 'serve']])

    def test_errors_are_messages(self):
        for error, code in ((validate.Unavailable('x'), 2), (dev.ToolError('x'), 1)):
            with mock.patch.object(dev, 'Host', side_effect=error):
                self.assertEqual(quiet(dev.main, ['host', 'build']), code)


class HostPlanTest(unittest.TestCase):
    def test_the_plan_has_the_steps_dev_uses(self):
        titles = [step.title for step in validate.plan_host('p', 'p', '1')]
        self.assertLessEqual({'configure', 'build', 'test', 'integration (verbose)'}, set(titles))


class HostTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.build = Path(folder.name)
        steps = [validate.Step(title, [title]) for title in ('configure', 'build', 'test', 'integration (verbose)')]
        plan = mock.Mock(return_value=steps)
        for target, name, value in ((validate, 'plan_host_win', plan), (validate, 'plan_host', plan),
                                    (validate, 'linux_probe', mock.Mock()), (dev, 'run', mock.Mock(return_value=0))):
            patch = mock.patch.object(target, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        self.tools = dev.Host()
        self.tools.build_dir = self.build

    def ran(self):
        return [call.args[0] for call in dev.run.call_args_list]

    def test_build_configures_only_a_new_directory(self):
        self.tools.build(['drivelink-gui'])
        (self.build / 'build.ninja').touch()
        self.tools.build()
        self.assertEqual(self.ran(), [['configure'], ['build', '--target', 'drivelink-gui'], ['build']])

    def test_failed_configure_stops(self):
        dev.run.return_value = 4
        self.assertEqual(self.tools.build(), 4)
        self.assertEqual(self.ran(), [['configure']])

    def test_every_test_or_a_filter(self):
        self.tools.test('')
        self.assertEqual(self.ran(), [['test'], ['integration (verbose)']])
        self.tools.test('^gui-')
        self.assertEqual(self.ran()[-1], ['ctest', '--preset', self.tools.preset, '-R', '^gui-', '--no-tests=error'])


class VdevTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.state = Path(folder.name) / 'vdev-state'
        for target, name, value in ((dev, 'VDEV_STATE', self.state), (dev, 'OUT', Path(folder.name)),
                                    (dev, 'Host', mock.Mock()), (dev, 'run', mock.Mock(return_value=0)),
                                    (jtag, 'listening', mock.Mock(return_value=False))):
            patch = mock.patch.object(target, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        dev.Host.return_value.build.return_value = 0
        dev.Host.return_value.program.side_effect = lambda name: name
        dev.Host.return_value.env = None

    def test_a_running_vdev_is_refused_and_its_state_kept(self):
        jtag.listening.return_value = True
        self.state.mkdir()
        for argv in (['vdev'], ['vdev', '--reset'], ['gui', '--vdev']):
            with self.subTest(argv=argv):
                self.assertEqual(quiet(dev.main, argv), 1)
        self.assertTrue(self.state.is_dir())
        dev.run.assert_not_called()

    def test_reset_removes_the_state_and_may_repeat(self):
        self.state.mkdir()
        (self.state / 'file').touch()
        for _ in range(2):
            self.assertEqual(quiet(dev.main, ['vdev', '--reset']), 0)
            self.assertFalse(self.state.exists())

    def test_gui_on_vdev_stops_the_vdev(self):
        device = types.SimpleNamespace()
        with mock.patch.object(dev, 'start_vdev', return_value=(device, 'tcp://127.0.0.1:8765')), \
                mock.patch.object(dev.package_common, 'stop') as stop:
            for outcome in (0, 5, OSError('x')):
                dev.run.side_effect = [outcome]
                quiet(dev.main, ['gui', '--vdev'])
                stop.assert_called_once_with(device)
                stop.reset_mock()
        command = dev.run.call_args.args[0]
        self.assertEqual(command[1:3], ['--endpoint', 'tcp://127.0.0.1:8765'])
        self.assertIn(str(dev.GUI_SETTINGS), command)  # never the user's settings

    def test_gui_on_a_board_starts_no_vdev(self):
        with mock.patch.object(dev, 'start_vdev') as start:
            quiet(dev.main, ['gui', '--endpoint', 'serial://COM3'])
            start.assert_not_called()
        self.assertEqual(dev.run.call_args.args[0], ['drivelink-gui', '--endpoint', 'serial://COM3'])

    def test_start_vdev_needs_ready(self):
        for output, endpoint in (('x\nREADY tcp://127.0.0.1:8765\n', 'tcp://127.0.0.1:8765'), ('bind failed\n', None)):
            process = types.SimpleNamespace(stdout=io.StringIO(output))
            with mock.patch.object(dev.subprocess, 'Popen', return_value=process), \
                    mock.patch.object(dev.package_common, 'stop') as stop:
                if endpoint:
                    self.assertEqual(quiet(dev.start_vdev, dev.Host()), (process, endpoint))
                    stop.assert_not_called()
                else:
                    with self.assertRaises(dev.ToolError):
                        quiet(dev.start_vdev, dev.Host())
                    stop.assert_called_once_with(process)


class TasksTest(unittest.TestCase):
    def setUp(self):
        text = (validate.ROOT / '.vscode/tasks.json').read_text(encoding='utf-8')
        config = json.loads(re.sub(r'^\s*//.*$', '', text, flags=re.MULTILINE))
        self.tasks = {task['label']: task for task in config['tasks']}
        self.assertEqual(len(self.tasks), len(config['tasks']))
        self.inputs = {entry['id']: entry for entry in config['inputs']}

    def script(self, task):
        prefix = '${workspaceFolder}/'
        self.assertTrue(task['args'][0].startswith(prefix), task['label'])
        return task['args'][0][len(prefix):]

    def test_every_task_runs_a_script_on_both_systems(self):
        for task in self.tasks.values():
            self.assertEqual((task['type'], task['command'], task['linux']),
                             ('process', 'python', {'command': 'python3'}))
            self.assertTrue((validate.ROOT / self.script(task)).is_file(), task['label'])
            self.assertIn('problemMatcher', task)  # without one VS Code asks at each run
            if 'dependsOn' in task:
                self.assertIn(task['dependsOn'], self.tasks)

    def test_inputs_exist_and_are_used(self):
        used = set(re.findall(r'\$\{input:(\w+)\}', json.dumps(self.tasks)))
        self.assertEqual(used, set(self.inputs))

    def test_arguments_are_valid(self):
        parsers = {'tools/dev.py': dev.arguments, 'tools/jtag.py': jtag.arguments}
        for task in self.tasks.values():
            parse = parsers.get(self.script(task))
            if parse:
                with self.subTest(task=task['label']):
                    parse(task['args'][1:])

    def test_bench_flash_tasks_ask_for_the_port(self):
        flashing = [task for task in self.tasks.values() if self.script(task) == 'tools/bench_fw.py']
        self.assertEqual(len(flashing), 2)
        for task in flashing:
            self.assertEqual(task['args'][2:], ['--port', '${input:flashPort}'])
            self.assertEqual(quiet(bench_fw.main, [*task['args'][1:], '--dry-run']), 0)

    def test_check_names_match_validate(self):
        names = [check.name for check in validate.CHECKS]
        self.assertEqual(self.inputs['check']['options'], names)
        for task in self.tasks.values():
            if '--only' in task['args']:
                self.assertIn(task['args'][task['args'].index('--only') + 1], [*names, '${input:check}'])

    def test_release_task_arguments_and_options_are_valid(self):
        options = self.inputs['releaseValidation']['options']
        self.assertLessEqual(set(options), set(release.VALIDATE_MODES))
        task = self.tasks['Release version']
        self.assertEqual(self.script(task), 'tools/release.py')
        for mode in options:
            argv = [arg.replace('${input:releaseVersion}', '0.2.2').replace('${input:releaseValidation}', mode)
                    for arg in task['args'][1:]]
            parsed = release.arguments(argv)
            self.assertEqual((parsed.version, parsed.validate), ('0.2.2', mode))

    def test_the_shortcuts_offer_a_list(self):
        groups = {label: task['group'] for label, task in self.tasks.items() if 'group' in task}
        self.assertEqual(groups, {'Firmware build': 'build', 'Host build': 'build', 'Host tests': 'test',
                                  'Host tests (filter)': 'test', 'Native tests': 'test', 'Validate quick': 'test'})

    def test_compiler_messages_are_found(self):
        pattern = re.compile(self.tasks['Host build']['problemMatcher']['pattern']['regexp'])
        self.assertEqual(pattern.match("E:/a b/x.cpp:12:3: error: 'y' was not declared").groups(),
                         ('E:/a b/x.cpp', '12', '3', 'error', "'y' was not declared"))
        self.assertEqual(pattern.match('../main/x.cpp:1:10: fatal error: z.h: No such file').group(4), 'error')
        self.assertIsNone(pattern.match('[12/40] Building CXX object x.cpp.obj'))

    def test_the_documentation_lists_every_task(self):
        text = (validate.ROOT / 'docs/development.md').read_text(encoding='utf-8')
        section = text.split('## VS Code tasks', 1)[1].split('\n## ', 1)[0]
        for label in self.tasks:
            self.assertIn(f'`{label}`', section)


if __name__ == '__main__':
    unittest.main()
