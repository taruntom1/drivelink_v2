"""Render real GUI shell scenarios; optionally repeat the vdev safety checks.

All provisioning and fault injection targets an isolated vdev. Use the app's
--endpoint/--out options directly for an existing physical device.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time

# Layout scenarios: the G7b three-loop sample device on vdev, built with the
# CLI through the real layout ops, then saved and rebooted. Type ids are the
# shipped mock and controller types; wiring is type<<8|instance and a source
# is type<<24|instance<<16|item (docs/protocol/core.md).
SAMPLE = [
    ['add', '5', 'fusion'],                      # loop 2, non-real-time
    ['set', '5:2:1=200'],
    ['add', '32', 'left_drive', '0'],            # controller.cascade_pid
    ['add', '112', 'left_pwm', '0'],             # actuator.mock
    ['add', '113', 'left_enc', '0'],             # feedback.mock
    ['add', '114', 'imu', '1'],                  # imu.mock in sensor
    ['add', '115', 'odom', '2'],                 # fusion.mock in fusion
    ['set', '32:0:1=%d' % (112 << 8), '32:0:2=%d' % (113 << 8)],
    ['set', '113:0:1=%d' % (112 << 8)],
    ['set', '115:0:1=%d' % ((113 << 24) | 0x0101), '115:0:2=%d' % ((114 << 24) | 0x0100)],
    ['save'],
]
# Staged after the reboot: a pending loop change, a live rate (unsaved),
# a pending add and a label change, for the pending and review screens.
STAGED = [
    ['set', '5:2:4=14'],
    ['set', '5:2:1=250'],
    ['add', '112', 'aux_pwm', '0'],
    ['label', '114', '0', 'imu_main'],
]
LAYOUT = {
    'layout': ('layout', '32:0', False),
    'layout-loop': ('layout', '5:2', True),
    'layout-sources': ('layout', '115:0', False),
    # H2c-2: live signals, an action's outcome, a stale source.
    'layout-signals': ('layout', '114:0', False),
    'layout-action': ('layout', '113:0', False),
    'layout-sources-stale': ('layout', '115:0', False),
    'layout-review': ('layout-review', '5:2', True),
    'layout-add-instance': ('layout-add-instance', '5:0', False),
    'layout-add-loop': ('layout-add-loop', '5:0', False),
}


# H2d Monitor on the sample device: loop statistics, the IMU and the
# controller on three plots; paused with A-B cursors; and a refused add:
# the sensor loop at 1 kHz (live), twelve controller signals at 1 kHz take
# about 80 % of the link, the IMU's first signal at 1 kHz would open a
# second 1 kHz group (over 100 %) and is refused; 500 Hz fits.
CONTROL = ','.join('32:0:%d' % item for item in
                   (0x100, 0x101, 0x102, 0x103, 0x110, 0x111, 0x112, 0x113, 0x114, 0x120, 0x121, 0x122))
# H2d-2 Manual command (G5): armed with left_drive's velocity_sp held at
# 1.5 rad/s (dragging, limit 2.5 as in G5) and duty_sp latched at 0.2; and
# the same card disarmed.
# Armed on a desktop vdev, the control loop runs at 100 Hz with a 1 s
# watchdog (as the native arming fixtures do): a 1 kHz loop can miss its
# stall deadline under load and latch a fault.
ARMED = [['set', '5:0:1=100', '0:0:4=1000']]
PLOTS = '5:0:258,5:0:259,114:0:256,114:0:272,32:0:257,115:0:260'
COMMANDS = '32:0:512@2.5=1.5,32:0:514=0.2:latch'
MONITOR = {
    'monitor': (ARMED, PLOTS, '', '10', COMMANDS),
    'monitor-paused': ([], PLOTS, '', '10', ''),
    'monitor-budget': ([['set', '5:1:1=1000']], CONTROL + ',114:0:256', '0:1,1:1', '5', ''),
    'monitor-command-disarmed': ([], PLOTS, '', '10', '32:0:512,32:0:514'),
}
# Scenarios that render a screen of another name.
SCREENS = {'safe': 'device', 'fault': 'drawer', 'lost': 'device', 'monitor-command-disarmed': 'monitor'}


# The action each scenario runs (left_enc's `sample`), and the loop stall
# (control, 8 s) that leaves odom's feedback source stale during capture.
ACTIONS = {'layout-action': '113:0:768'}
STALLS = {'layout-sources-stale': 'loop-stall 0 8000'}


def cli_run(cli, endpoint, args, attempts=1):
    for attempt in range(attempts):
        result = subprocess.run([cli, '--endpoint', endpoint, *args], capture_output=True, text=True, timeout=30)
        if result.returncode == 0:
            return
        time.sleep(0.5)
    raise AssertionError((args, result.returncode, result.stdout, result.stderr))


def provision(cli, endpoint, staged):
    for command in SAMPLE:
        cli_run(cli, endpoint, command)
    # vdev closes TCP after the reboot reply; the next command waits for it.
    subprocess.run([cli, '--endpoint', endpoint, 'reboot'], capture_output=True, text=True, timeout=30)
    cli_run(cli, endpoint, ['info'], attempts=20)
    if staged:
        for command in STAGED:
            cli_run(cli, endpoint, command)


def write_image(path, seed, version, data_bytes):
    """A structurally valid, non-executable app image for vdev (never flash it)."""
    end = (32 + data_bytes + 16) & ~15
    data = bytearray(end + 32)
    data[0] = 0xe9; data[1] = 1; data[12] = 9; data[23] = 1
    struct.pack_into('<II', data, 24, 0x3c000020, data_bytes)
    struct.pack_into('<I', data, 32, 0xabcd5432)
    data[48:48 + len(version)] = version.encode()
    data[80:92] = b'drivelink_v2'
    data[176:208] = bytes((seed + i) & 255 for i in range(32))
    for i in range(288, 32 + data_bytes):
        data[i] = (i + seed) & 255
    checksum = 0xef
    for value in data[32:32 + data_bytes]:
        checksum ^= value
    data[end - 1] = checksum
    data[end:] = hashlib.sha256(data[:end]).digest()
    path.write_bytes(data)
    return path


def render(gui, vdev, destination, scenario, cli=None):
    with tempfile.TemporaryDirectory(prefix='drivelink-gui-') as work:
        control = Path(work) / 'control'
        device = None
        endpoint = None
        try:
            if scenario != 'connect':
                args = [vdev, '--port', '0', '--state', str(Path(work) / 'state'),
                        '--scenario-file', str(control)]
                if scenario == 'safe':
                    args.append('--safe-mode')
                device = subprocess.Popen(args, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True)
                startup = threading.Timer(10, device.kill)
                startup.start()
                try:
                    line = device.stdout.readline().strip()
                finally:
                    startup.cancel()
                assert line.startswith('READY '), (line, device.poll())
                endpoint = line[6:]
            screen = SCREENS.get(scenario, scenario)
            select = None
            if scenario in LAYOUT:
                screen, select, staged = LAYOUT[scenario]
                provision(cli, endpoint, staged)
            if scenario in MONITOR:
                provision(cli, endpoint, False)
                for command in MONITOR[scenario][0]:
                    cli_run(cli, endpoint, command)
            out = destination / scenario
            # Isolated recent devices; let loop run time arrive before capture.
            command = [gui, '--screen', screen, '--out', str(out),
                       '--settings', str(Path(work) / 'gui.ini'),
                       '--settle', '0' if scenario in ('connect', 'lost') else '1500']
            if endpoint:
                command += ['--endpoint', endpoint]
            if select:
                command += ['--select', select]
            if scenario in MONITOR:
                _, plots, rates, window, commands = MONITOR[scenario]
                command += ['--plot', plots, '--window', window]
                if rates:
                    command += ['--rate', rates]
                if commands:
                    command += ['--command', commands]
            if scenario in ACTIONS:
                command += ['--action', ACTIONS[scenario]]
            if scenario.startswith('update'):
                # Large enough that the writing screen holds mid-transfer.
                command += ['--image', str(write_image(Path(work) / 'drivelink_fw_2.1.0.bin', 77, '2.1.0', 1 << 20))]
            if scenario in ('safe', 'fault', 'lost'):
                command += ['--expect-state', scenario]
            app = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
            errors = []
            thread = threading.Thread(target=lambda: errors.append(app.stderr.read()), daemon=True)
            thread.start()
            timer = threading.Timer(40, app.kill)
            timer.start()
            output = []
            try:
                for line in app.stdout:
                    output.append(line)
                    if line.strip() == 'GUI READY' and scenario in ('fault', 'lost', *STALLS):
                        next_file = Path(work) / 'next'
                        injection = STALLS.get(scenario) or ('fault' if scenario == 'fault' else 'stall')
                        next_file.write_text('1 ' + injection + '\n')
                        os.replace(next_file, control)
                code = app.wait(timeout=5)
            finally:
                timer.cancel()
                if app.poll() is None:
                    app.kill()
                    app.wait(timeout=5)
                thread.join(timeout=5)
            assert code == 0, (scenario, output, errors)
            if os.environ.get('DRIVELINK_SCENARIO_LOG'):
                print(scenario, ''.join(errors), flush=True)
            images = sorted(out.glob('*.png'))
            if scenario in LAYOUT or scenario in MONITOR:
                # The file names carry the screen, not the scenario.
                assert all(image.name.startswith(screen) for image in images), images
            assert len(images) == 4, (scenario, images, output, errors)
            dimensions = []
            for image in images:
                header = image.read_bytes()[:24]
                assert header[:8] == b'\x89PNG\r\n\x1a\n'
                dimensions.append(struct.unpack('>II', header[16:24]))
            assert sorted(dimensions) == [(1920, 1080)] * 2 + [(2560, 1440)] * 2
            print('PASS shell screenshots:', scenario, flush=True)
        finally:
            if device is not None:
                device.kill()
                device.communicate(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gui', required=True)
    parser.add_argument('--vdev', required=True)
    parser.add_argument('--backend', help='Repeat backend/safety tests before rendering')
    parser.add_argument('--cli', help='drivelink-cli for the Layout fixtures (default: next to vdev)')
    parser.add_argument('--out', required=True)
    parser.add_argument('--smoke', action='store_true', help='Only Connect and device shell')
    parser.add_argument('--only', nargs='+', help='Render only these scenarios')
    args = parser.parse_args()
    os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
    os.environ.setdefault('QT_QUICK_BACKEND', 'software')
    os.environ.setdefault('QT_FORCE_STDERR_LOGGING', '1')
    if args.backend:
        subprocess.run([args.backend], check=True, timeout=300)
    vdev = Path(args.vdev)
    cli = args.cli or str(vdev.parent.parent / 'cli' / ('drivelink-cli' + vdev.suffix))
    destination = Path(args.out).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    for scenario in (['connect', 'device'] if args.smoke else
                     ['connect', 'device', *LAYOUT, 'tune', *MONITOR, 'drawer', 'dialog', 'safe', 'fault', 'lost',
                      'rename', 'reset', 'unsaved', 'notice',
                      'update', 'update-writing', 'update-confirmed', 'update-rolledback']):
        if not args.only or scenario in args.only:
            render(args.gui, args.vdev, destination, scenario, cli)


if __name__ == '__main__':
    main()
