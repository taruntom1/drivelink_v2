#!/usr/bin/env python3
"""Everyday development commands, with each toolchain set up as validate.py does.

  fw build|menuconfig|size|fullclean   idf.py on firmware/ (normal build)
  fw monitor [--port COM4]             the console; Ctrl+] ends it
  host configure|build [TARGET...]     this OS's debug preset of host/superbuild
  host test [REGEX]                    build, then every test or those matching
  vdev [--reset]                       the virtual device on tcp://127.0.0.1:8765
  gui [--vdev | --endpoint URL]        build and open the GUI; --vdev starts (and
                                       then stops) a virtual device for it
  cli ARGS...                          build and run drivelink-cli
  guide setup|build|serve              the user guide, in build/guide-venv
  notices                              rewrite the third-party notices

The VS Code tasks (.vscode/tasks.json) call these. vdev state and the --vdev
GUI's settings are kept in build/dev/. Flash with tools/bench_fw.py: nothing
here writes to a board, except what you do in the GUI or ask the CLI for.

Exit codes: 0 ok, 1 failed or a tool's own code, 2 a prerequisite is missing,
130 Ctrl+C.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading

import jtag
import package_common
import package_win
import validate

ROOT = validate.ROOT
WINDOWS = validate.WINDOWS
OUT = ROOT / 'build' / 'dev'  # ignored by .gitignore (build/)
VDEV_STATE = OUT / 'vdev-state'
GUI_SETTINGS = OUT / 'gui.ini'
VDEV_PORT = 8765  # vdev's default
VENV = ROOT / 'build' / 'guide-venv'
VENV_PYTHON = VENV / ('Scripts/python.exe' if WINDOWS else 'bin/python')
IDF_ACTIONS = {'build': ['build'], 'menuconfig': ['menuconfig'], 'size': ['size-components'],
               'fullclean': ['fullclean']}
NOTICES = [[], *(['--manifest', f'{folder}/notices.json', '--output', f'{folder}/THIRD_PARTY_NOTICES.txt']
                 for folder in ('firmware/notices', 'host/notices'))]  # []: the GUI's, the script's default


class ToolError(Exception):
    """A clean error message for the user."""


def run(argv, cwd=ROOT, env=None):
    """Run in this terminal. Ctrl+C goes to the program; wait for it to end."""
    env = dict(os.environ) if env is None else env
    program = shutil.which(str(argv[0]), path=env.get('PATH'))
    if not program:
        raise validate.Unavailable(f'{argv[0]} not found')
    print('$ ' + ' '.join(map(str, argv)), file=sys.stderr, flush=True)
    process = subprocess.Popen([program, *map(str, argv[1:])], cwd=cwd, env=env)
    while True:
        try:
            return process.wait()
        except KeyboardInterrupt:
            pass


def run_all(steps):
    """Run validate.Steps until one fails."""
    for step in steps:
        code = run(step.argv, step.cwd, step.env)
        if code:
            return code
    return 0


# --- Firmware ---------------------------------------------------------------

def idf_command(args):
    if args.action == 'monitor':
        # --no-reset: opening the console does not reset the board on purpose.
        return ['-p', args.port, 'monitor', '--no-reset']
    return IDF_ACTIONS[args.action]


def firmware(args):
    env = validate.idf_env()
    idf_py = str(Path(env['IDF_PATH']) / 'tools' / 'idf.py')
    return run(['python', idf_py, *idf_command(args)], ROOT / 'firmware', env)


# --- Host -------------------------------------------------------------------

class Host:
    """This OS's debug preset, with the steps of validate.py's host check."""

    def __init__(self):
        jobs = str(os.cpu_count() or 4)
        if WINDOWS:
            self.preset, steps = 'windows-mingw', validate.plan_host_win(jobs)
        else:
            validate.linux_probe()
            self.preset, steps = 'linux', validate.plan_host('linux', 'linux', jobs)
        self.steps = {step.title: step for step in steps}
        self.build_dir = ROOT / 'host/superbuild/build' / self.preset
        self.env = dict(self.steps['build'].env or os.environ)
        if WINDOWS:  # the Qt DLLs, for running the programs
            self.env['PATH'] = f"{package_win.QT_ROOT / 'bin'};{self.env['PATH']}"

    def program(self, name):
        folder = name.removeprefix('drivelink-')
        return str(self.build_dir / folder / (name + ('.exe' if WINDOWS else '')))

    def configure(self):
        return run_all([self.steps['configure']])

    def build(self, targets=()):
        if not (self.build_dir / 'build.ninja').is_file():
            code = self.configure()
            if code:
                return code
        step = self.steps['build']
        return run([*step.argv, *(['--target', *targets] if targets else [])], step.cwd, step.env)

    def test(self, regex=''):
        if not regex:
            return run_all([self.steps['test'], self.steps['integration (verbose)']])
        step = self.steps['test']
        return run(['ctest', '--preset', self.preset, '-R', regex, '--no-tests=error'], step.cwd, step.env)


def host(args):
    if args.action == 'test' and len(args.operands) > 1:
        raise ToolError('host test takes one regex')
    tools = Host()
    if args.action == 'configure':
        return tools.configure()
    if args.action == 'build':
        return tools.build(args.operands)
    return tools.build() or tools.test(*args.operands)


def vdev_command(tools):
    return [tools.program('drivelink-vdev'), '--state', str(VDEV_STATE), '--port', str(VDEV_PORT)]


def need_vdev_port():
    # Two vdevs would share the state, and on Windows even the port.
    if jtag.listening(VDEV_PORT):
        raise ToolError(f'port {VDEV_PORT} is in use (another vdev? stop it first)')


def vdev(args):
    need_vdev_port()
    if args.reset:
        if VDEV_STATE.exists():
            shutil.rmtree(VDEV_STATE)
        print(f'vdev state removed: {VDEV_STATE}')
        return 0
    tools = Host()
    code = tools.build(['drivelink-vdev'])
    if code:
        return code
    OUT.mkdir(parents=True, exist_ok=True)
    return run(vdev_command(tools), env=tools.env)


def start_vdev(tools):
    """Start vdev, echoing its output. Returns it and its endpoint once READY."""
    process = subprocess.Popen(vdev_command(tools), cwd=ROOT, env=tools.env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors='replace')

    def echo():
        for line in process.stdout:
            print(line, end='', flush=True)

    for line in process.stdout:
        print(line, end='', flush=True)
        if line.startswith('READY '):
            threading.Thread(target=echo, daemon=True).start()
            return process, line.split(None, 1)[1].strip()
    package_common.stop(process)
    raise ToolError('drivelink-vdev did not print READY')


def gui(args):
    if args.vdev:
        need_vdev_port()
    tools = Host()
    code = tools.build(['drivelink-gui', *(['drivelink-vdev'] if args.vdev else [])])
    if code:
        return code
    program = tools.program('drivelink-gui')
    if not args.vdev:
        return run([program, *(['--endpoint', args.endpoint] if args.endpoint else [])], env=tools.env)
    OUT.mkdir(parents=True, exist_ok=True)
    device, endpoint = start_vdev(tools)
    try:
        return run([program, '--endpoint', endpoint, '--settings', str(GUI_SETTINGS)], env=tools.env)
    finally:
        package_common.stop(device)


def cli(args):
    tools = Host()
    return (tools.build(['drivelink-cli'])
            or run([tools.program('drivelink-cli'), *args.args], env=tools.env))


# --- Guide and notices ------------------------------------------------------

def guide(args):
    if args.action == 'setup' or not VENV_PYTHON.is_file():
        code = (0 if VENV_PYTHON.is_file() else run([sys.executable, '-m', 'venv', VENV])) or run(
            [VENV_PYTHON, '-m', 'pip', 'install', '-r', 'guide/requirements.txt'])
        if code or args.action == 'setup':
            return code
    return run([VENV_PYTHON, 'tools/guide.py', args.action])


def notices(args):
    return run_all([validate.Step('notices', [sys.executable, 'host/gui/notices/gen_notices.py', '--write', *options])
                    for options in NOTICES])


def arguments(argv):
    if argv and argv[0] == 'cli':  # every argument is drivelink-cli's
        return argparse.Namespace(handler=cli, args=argv[1:])
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    command = sub.add_parser('fw')
    command.add_argument('action', choices=[*IDF_ACTIONS, 'monitor'])
    command.add_argument('--port', default='COM4' if WINDOWS else '/dev/ttyACM0', help='the console port')
    command.set_defaults(handler=firmware)
    command = sub.add_parser('host')
    command.add_argument('action', choices=['configure', 'build', 'test'])
    command.add_argument('operands', nargs='*', help='targets to build, or a ctest regex')
    command.set_defaults(handler=host)
    command = sub.add_parser('vdev')
    command.add_argument('--reset', action='store_true', help='remove the kept state instead of starting it')
    command.set_defaults(handler=vdev)
    command = sub.add_parser('gui')
    device = command.add_mutually_exclusive_group()
    device.add_argument('--vdev', action='store_true', help='on a virtual device of its own')
    device.add_argument('--endpoint', help='connect to this device (serial://COM3, tcp://HOST:PORT)')
    command.set_defaults(handler=gui)
    sub.add_parser('cli', help='every argument goes to drivelink-cli')
    command = sub.add_parser('guide')
    command.add_argument('action', choices=['setup', 'build', 'serve'])
    command.set_defaults(handler=guide)
    sub.add_parser('notices').set_defaults(handler=notices)
    return parser.parse_args(argv)


def main(argv=None):
    args = arguments(sys.argv[1:] if argv is None else argv)
    try:
        return args.handler(args)
    except validate.Unavailable as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 2
    except (ToolError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
