"""Render the G6 remote-mode screens of drivelink-gui from real fixtures (Linux only).

Each scenario starts an isolated vdev (the G7b sample device, provisioned once
and copied), drivelink-agent in front of it, optionally raw agent clients that
play ROS or a second GUI (agent_integration.RawClient), and runs
`drivelink-gui --agent ... --out` once. Four PNGs per scenario (2560x1440 and
1920x1080, dark and light) land in OUT/<scenario>/. Not a ctest: run it by hand
(WSL on Windows) and compare the images with the G6 frames.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'agent' / 'test'))
import scenarios as local          # noqa: E402  the vdev provisioning helpers
import agent_integration as ai     # noqa: E402  RawClient and the agent helpers

ALIAS = 'base'
OFFLINE_ALIAS = 'arm'
BASE_GUI = 'bench-pc'              # this window's name for the other clients
# The velocity gain of left_drive (a tune-flagged live param), and the Monitor plots of scenarios.py.
LIVE_EDIT = local.VEL_KP + '=0.1'
TUNE_ARGS = ['--tune', '32:0']
MONITOR_ARGS = ['--plot', local.PLOTS, '--window', '5']

# name -> (screen, extra GUI args, holder, flags)
#   holder: None, or (client name, AgentClientKind, armed)
#   flags:  'take' = --take-lease, 'gui_takes' = another GUI takes it once this one holds it,
#           'history' = lease and arm changes before the GUI connects, 'offline' = a second, stopped device,
#           'closed' = an address nobody listens on, 'badtoken'
# The Agent tab's "Found on the network" (H3b-2) never touches the network in a scenario:
# DRIVELINK_GUI_FAKE_AGENTS carries the agents it shows (FAKE_AGENTS, else none).
ROS = ('ros_bridge', ai.KIND_ROS, True)
LAPTOP = ('bench-laptop', ai.KIND_GUI, True)
SCENARIOS = {
    'remote-agent': ('agent-connect', [], None, ()),
    'remote-unreachable': ('agent-failure', [], None, ('closed',)),
    'remote-no-device': ('agent-failure', [], None, ('offline',)),
    'remote-no-device-list': ('agent-connect', [], None, ('offline',)),
    'remote-agent-found': ('agent-connect', [], None, ()),
    'remote-agent-found-empty': ('agent-connect', [], None, ()),
    'remote-bad-token': ('agent-failure', [], None, ('badtoken',)),
    'remote-device-ros': ('device', [], ROS, ()),
    'remote-tune-ros': ('tune', TUNE_ARGS, ROS, ()),
    'remote-monitor-observer': ('monitor', MONITOR_ARGS, ROS, ()),
    'remote-take-ros': ('lease-take', [], ROS, ()),
    'remote-live-edit': ('live-edit', TUNE_ARGS + ['--live-edit', LIVE_EDIT], ROS, ()),
    'remote-available': ('device', [], None, ()),
    'remote-granted': ('tune', TUNE_ARGS, None, ('take',)),
    'remote-release': ('lease-release', TUNE_ARGS, None, ('take',)),
    'remote-revoked': ('revoked', TUNE_ARGS, None, ('take', 'gui_takes')),
    'remote-other-gui': ('monitor', MONITOR_ARGS, LAPTOP, ()),
    'remote-history': ('drawer', [], None, ('history',)),
}
FAKE_AGENTS = {
    'remote-agent-found': [
        {'instance': 'robot-pi', 'host': 'robot-pi.local', 'address': '192.168.1.20:47620', 'version': '0.1.0', 'proto': 3},
        {'instance': 'bench-wsl', 'host': 'bench-wsl.local', 'address': '192.168.1.42:47620', 'version': '0.1.0', 'proto': 3},
        {'instance': 'old-agent', 'host': 'old-agent.local', 'address': '192.168.1.50:47620', 'version': '0.0.9', 'proto': 2},
    ],
    'remote-agent-found-empty': [],
}
DIALOGS = ('lease-take', 'lease-release', 'live-edit', 'revoked')


class Keepalive:
    """A raw client that holds the lease and keeps its command group alive (the
    device's watchdog would disarm it otherwise) until it is stopped."""

    def __init__(self, client, epoch):
        self.client, self.epoch = client, epoch
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stop_flag.is_set():
                self.client.keepalives(self.epoch, 0.25)
        except (ai.Fail, OSError):
            pass

    def stop(self):
        self.stop_flag.set()
        self.thread.join(timeout=5)
        self.client.close()


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def take_lease(client):
    result, detail, _ = client.lease_take()
    ai.expect(result == ai.R_OK, f'{client.name}: LeaseTake', result=result, detail=detail)
    ai.leases(client, 1)


def hold(ctx, name, kind, armed):
    """A client of `base` that takes the lease and, if asked, arms the device."""
    client = ai.RawClient(name, ALIAS, kind=kind)
    take_lease(client)
    if not armed:
        return Plain(client)
    return Keepalive(client, ai.config_and_arm(ctx, client))


class Plain:
    """A holder that only holds the lease (it reads, so the agent's queue never fills)."""

    def __init__(self, client):
        self.client = client
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stop_flag.is_set():
                self.client.pump(0.25, eof_ok=True)
        except (ai.Fail, OSError):
            pass

    def stop(self):
        self.stop_flag.set()
        self.thread.join(timeout=5)
        self.client.close()


def make_template(work, vdev, cli):
    """The sample device with the Tune setup, the armed-loop rates and a saved layout."""
    state = work / 'template'
    proc = subprocess.Popen([vdev, '--port', '0', '--state', str(state)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    timer = threading.Timer(15, proc.kill)
    timer.start()
    try:
        line = proc.stdout.readline().strip()
    finally:
        timer.cancel()
    assert line.startswith('READY '), (line, proc.poll())
    endpoint = line[6:]
    try:
        local.provision(cli, endpoint, False)
        for command in local.TUNE_SETUP:
            local.cli_run(cli, endpoint, command)
        subprocess.run([cli, '--endpoint', endpoint, 'reboot'], capture_output=True, text=True, timeout=30)
        local.cli_run(cli, endpoint, ['info'], attempts=20)
        for command in local.ARMED:
            local.cli_run(cli, endpoint, command)
        local.cli_run(cli, endpoint, ['save'])
    finally:
        proc.kill()
        proc.communicate(timeout=10)
    return state


def run_gui(args, command, on_line, env=None):
    """Run the GUI, handing every stdout line to on_line; return (code, stdout lines, stderr)."""
    app = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    errors = []
    thread = threading.Thread(target=lambda: errors.append(app.stderr.read()), daemon=True)
    thread.start()
    timer = threading.Timer(60, app.kill)
    timer.start()
    output = []
    try:
        for line in app.stdout:
            output.append(line.rstrip('\n'))
            on_line(line.strip())
        code = app.wait(timeout=10)
    finally:
        timer.cancel()
        if app.poll() is None:
            app.kill()
            app.wait(timeout=5)
        thread.join(timeout=5)
    return code, output, ''.join(errors)


def render(args, work, template, destination, name):
    screen, extra, holder, flags = SCENARIOS[name]
    ai.AGENT_URL = None
    ai.PROCS.clear()
    ai.CLI = args.cli
    ai.TOKEN = secrets.token_hex(12)
    holders = []
    try:
        state = work / f'state-{name}'
        shutil.copytree(template, state)
        vdev, vdev_url = ai.start_vdev('vdev-' + name, args.vdev, state)
        command = [args.agent_bin, '--port', '0', '--token', ai.TOKEN, '--device', f'{ALIAS}={vdev_url}',
                   '--retry-ms', '300', '--hello-timeout-ms', '2000']
        offline = 'offline' in flags
        if offline:
            # A device whose vdev is not running: the agent keeps retrying, the listing shows it offline.
            command += ['--device', f'{OFFLINE_ALIAS}=tcp://127.0.0.1:{free_port()}']
        agent = ai.Proc('agent-' + name, command)
        ai.AGENT_URL = agent.next_line(15, 'READY ')[len('READY '):]
        ai.wait_devices(lambda d: d.get(ALIAS, {}).get('online'), 'the device to come online', timeout=30)
        ctx = {'cmd_ref': (112, 0, 512)}
        if 'history' in flags:
            # A few lease and arm changes for the agent's history (sent to the GUI at connect).
            ros = hold(ctx, 'ros_bridge', ai.KIND_ROS, True)
            time.sleep(1.0)
            ros.stop()
            ai.settle(ctx, ALIAS, timeout=15)
            laptop = hold(ctx, 'bench-laptop', ai.KIND_GUI, False)
            time.sleep(0.5)
            laptop.stop()
            ai.settle(ctx, ALIAS, timeout=15)
        elif holder:
            holders.append(hold(ctx, *holder))
        agent_address = ai.AGENT_URL[len('tcp://'):]
        token = ai.TOKEN
        device = ALIAS
        if 'closed' in flags:
            agent_address = f'127.0.0.1:{free_port()}'
        if 'badtoken' in flags:
            token = '0' * 24
        if 'offline' in flags and screen == 'agent-failure':
            device = OFFLINE_ALIAS
        out = destination / name
        gui = [args.gui, '--screen', screen, '--out', str(out), '--settings', str(work / f'gui-{name}.ini'),
               '--client-name', BASE_GUI, '--agent', agent_address, '--token', token,
               '--settle', '1500' if screen not in DIALOGS and screen not in ('agent-connect', 'agent-failure') else '1000']
        if screen != 'agent-connect':
            gui += ['--device', device]
        if 'take' in flags:
            gui += ['--take-lease']
        gui += extra
        other = []

        def on_line(line):
            if line == 'GUI LEASE MINE' and 'gui_takes' in flags and not other:
                # Another GUI takes the lease from this window (the agent disarms the device first).
                client = ai.RawClient('bench-laptop', ALIAS, kind=ai.KIND_GUI)
                other.append(client)
                result, detail, _ = client.lease_take()
                ai.expect(result == ai.R_OK, 'bench-laptop: LeaseTake', result=result, detail=detail)

        env = dict(os.environ, DRIVELINK_GUI_FAKE_AGENTS=json.dumps(FAKE_AGENTS.get(name, [])))
        code, output, errors = run_gui(args, gui, on_line, env)
        assert code == 0, (name, code, output, errors)
        if os.environ.get('DRIVELINK_SCENARIO_LOG'):
            print(name, errors, flush=True)
        for client in other:
            client.close()
        images = sorted(out.glob('*.png'))
        assert len(images) == 4, (name, images, output, errors)
        for image in images:
            header = image.read_bytes()[:24]
            assert header[:8] == b'\x89PNG\r\n\x1a\n'
            size = struct.unpack('>II', header[16:24])
            assert size in ((2560, 1440), (1920, 1080)), (image, size)
        print('PASS remote screenshots:', name, flush=True)
    finally:
        for h in holders:
            h.stop()
        for proc in ai.PROCS:
            proc.kill()
        ai.PROCS.clear()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gui', required=True, help='drivelink-gui')
    parser.add_argument('--vdev', required=True, help='drivelink-vdev')
    parser.add_argument('--agent', required=True, help='drivelink-agent')
    parser.add_argument('--cli', help='drivelink-cli (default: next to vdev)')
    parser.add_argument('--out', required=True)
    parser.add_argument('--only', nargs='+', help='Render only these scenarios')
    args = parser.parse_args()
    if not sys.platform.startswith('linux'):
        print('SKIP remote screenshots are Linux only')
        return 0
    vdev = Path(args.vdev)
    args.agent_bin = args.agent
    args.cli = args.cli or str(vdev.parent.parent / 'cli' / 'drivelink-cli')
    os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
    os.environ.setdefault('QT_QUICK_BACKEND', 'software')
    os.environ.setdefault('QT_FORCE_STDERR_LOGGING', '1')
    destination = Path(args.out).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    names = [n for n in SCENARIOS if not args.only or n in args.only]
    unknown = [n for n in (args.only or []) if n not in SCENARIOS]
    if unknown:
        print('Unknown scenario(s):', ', '.join(unknown), file=sys.stderr)
        return 2
    failed = []
    with tempfile.TemporaryDirectory(prefix='drivelink-remote-shots-') as work:
        work = Path(work)
        template = make_template(work, args.vdev, args.cli)
        for name in names:
            try:
                render(args, work, template, destination, name)
            except (AssertionError, ai.Fail, subprocess.SubprocessError, OSError) as failure:
                failed.append(name)
                print(f'FAIL remote screenshots: {name}: {failure}', file=sys.stderr, flush=True)
    if failed:
        print('Failed:', ', '.join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
