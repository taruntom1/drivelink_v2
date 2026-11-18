"""H2g: the installed GUI on every screen against a device, then restoration.

    python host/test/installed_gui_smoke.py --installer build/installer/DriveLink-<v>-setup.exe \
        --endpoint serial://COM3 --out <dir>
    python host/test/installed_gui_smoke.py --installer <setup.exe> --vdev --out <dir>

Installs per user into a temporary directory (silent), records the device's
layout and loop rates, adds a mock layout (actuator, feedback and a controller
in the control loop, imu.mock in the sensor loop), saves and reboots, then
renders each screen with the installed drivelink-gui.exe (Qt and MinGW off
PATH), both sizes and themes. Afterwards it removes the mocks, restores the
rates, saves, reboots and checks the layout, rates and flags match the start;
then uninstalls. Mock modules only: never run it with motors or sensors
attached. It refuses a device with unsaved changes, pending edits, safe mode or
an armed state. The fixtures use the installed drivelink-cli; with --vdev the
installed drivelink-vdev is the device (the dry run).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import threading
import time

RATE_REFS = ['5:0:1', '5:1:1']
# (screen, extra GUI options): every screen a physical device allows.
SCREENS = [
    ('connect', []),
    ('device', []),
    ('drawer', []),
    ('layout', ['--select', '32:{controller}']),
    ('layout', ['--select', '114:{imu}', '--panel'], 'layout-calibration'),
    ('tune', ['--tune', '32:{controller}']),
    # Loop run times, the IMU's first signal and the controller's velocity.
    ('monitor', ['--plot', '5:0:258,5:1:258,114:{imu}:256,32:{controller}:257', '--window', '10']),
    ('settings', []),
]


class Cli:
    def __init__(self, path, endpoint):
        self.path, self.endpoint = str(path), endpoint

    def __call__(self, *args, code=0, timeout=120):
        done = subprocess.run([self.path, '--endpoint', self.endpoint, '--json', *map(str, args)],
                              capture_output=True, text=True, timeout=timeout, env=clean_env())
        data = json.loads(done.stdout)
        assert done.returncode == code, (args, done.returncode, data, done.stderr[-800:])
        return data.get('data', data)

    def connected(self, attempts=30):
        for _ in range(attempts):
            try:
                return self('info')
            except (AssertionError, json.JSONDecodeError, subprocess.TimeoutExpired):
                time.sleep(1)
        raise AssertionError('the device did not come back')

    def reboot(self):
        self('reboot')
        time.sleep(2)
        return self.connected()


def clean_env():
    """No Qt, MinGW or MSYS on PATH: the installed copy must be self-contained."""
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('QT_')}
    windows = os.environ.get('SystemRoot', r'C:\Windows')
    env['PATH'] = f'{windows}\\System32;{windows}'
    return env


def install(setup, directory, log):
    subprocess.run([str(setup), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/CURRENTUSER', '/NOICONS',
                    f'/DIR={directory}', f'/LOG={log}'], check=True, timeout=300)
    gui = directory / 'drivelink-gui.exe'
    for name in ('drivelink-gui.exe', 'drivelink-cli.exe', 'drivelink-vdev.exe',
                 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt'):
        assert (directory / name).is_file(), f'{name} missing after install'
    return gui


def uninstall(directory):
    uninstaller = directory / 'unins000.exe'
    if uninstaller.is_file():
        subprocess.run([str(uninstaller), '/VERYSILENT', '/SUPPRESSMSGBOXES'], timeout=300)
        for _ in range(30):  # the uninstaller finishes in a copy of itself
            if not (directory / 'drivelink-gui.exe').exists():
                break
            time.sleep(1)
    assert not (directory / 'drivelink-gui.exe').exists(), 'uninstall left the GUI behind'


def render(gui, out, screen, options, endpoint, settings, name):
    target = out / name
    command = [str(gui), '--screen', screen, '--out', str(target), '--settings', str(settings),
               '--settle', '0' if screen in ('connect', 'settings') else '2000']
    if screen != 'connect':
        command += ['--endpoint', endpoint]
    command += options
    started = time.monotonic()
    done = subprocess.run(command, capture_output=True, text=True, timeout=90, env=clean_env())
    assert done.returncode == 0, (name, done.returncode, done.stdout[-800:], done.stderr[-1500:])
    images = sorted(target.glob('*.png'))
    sizes = []
    for image in images:
        header = image.read_bytes()[:24]
        assert header[:8] == b'\x89PNG\r\n\x1a\n', image
        sizes.append(struct.unpack('>II', header[16:24]))
    assert sorted(sizes) == [(1920, 1080)] * 2 + [(2560, 1440)] * 2, (name, images)
    print(f'PASS {name}: 4 images in {time.monotonic() - started:.1f} s', flush=True)


def start_vdev(path, work):
    device = subprocess.Popen([str(path), '--port', '0', '--state', str(work / 'state')], env=clean_env(),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    timer = threading.Timer(10, device.kill)
    timer.start()
    try:
        line = device.stdout.readline().strip()
    finally:
        timer.cancel()
    assert line.startswith('READY '), (line, device.poll())
    return device, line[6:]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--installer', required=True, type=Path)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--endpoint', help='the device, e.g. serial://COM3')
    target.add_argument('--vdev', action='store_true', help='dry run: the installed drivelink-vdev')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='drivelink-installed-'))
    device = None
    directory = work / 'DriveLink'
    try:
        gui = install(args.installer, directory, work / 'install.log')
        print(f'installed {args.installer.name} into {directory}', flush=True)
        if args.vdev:
            device, endpoint = start_vdev(directory / 'drivelink-vdev.exe', work)
        else:
            endpoint = args.endpoint
        cli = Cli(directory / 'drivelink-cli.exe', endpoint)
        info = cli.connected()
        # The same refusals as hardware_smoke.py: pending edits, invalid layout
        # or safe mode (info flags), unsaved edits (status flag 8), armed.
        assert not info['flags'] & 11, 'pending edits, invalid layout or safe mode; nothing was changed'
        status = cli('status')
        assert not status['flags'] & 8, 'unsaved parameter edits; nothing was changed'
        assert status.get('arm_state', 0) == 0, 'the device is armed; nothing was changed'
        original = cli('instances')
        rates = cli('get', *RATE_REFS)['typed_values']
        print(f'device {info.get("device_id")} firmware {info["firmware_version"]}: '
              f'{len(original)} instances, rates {rates}', flush=True)

        created = []
        try:
            ids = {}
            for key, type_, label, loop in [('actuator', 112, 'h2g_pwm', 0), ('encoder', 113, 'h2g_enc', 0),
                                            ('controller', 32, 'h2g_drive', 0), ('imu', 114, 'h2g_imu', 1)]:
                ids[key] = cli('add', type_, label, loop)['instance']
                created.append((type_, ids[key]))
            cli('set', f'32:{ids["controller"]}:1={(112 << 8) | ids["actuator"]}',
                f'32:{ids["controller"]}:2={(113 << 8) | ids["encoder"]}')
            cli('set', f'113:{ids["encoder"]}:1={(112 << 8) | ids["actuator"]}')
            cli('save')
            cli.reboot()
            assert len(cli('instances')) == len(original) + len(created), 'the mock layout did not load'

            settings = work / 'gui.ini'
            for entry in SCREENS:
                screen, options = entry[0], [o.format(**ids) for o in entry[1]]
                name = entry[2] if len(entry) > 2 else screen
                render(gui, args.out, screen, options, endpoint, settings, name)
                if screen != 'connect':
                    cli.connected()  # the device is still there after the GUI let go
        finally:
            for type_, instance in reversed(created):
                cli('remove', type_, instance)
            cli('set', *(f'{ref}={value}' for ref, value in zip(RATE_REFS, rates)))
            cli('save')
            cli.reboot()
        restored = cli('instances')
        assert restored == original, ('layout not restored', original, restored)
        assert cli('get', *RATE_REFS)['typed_values'] == rates, 'loop rates not restored'
        after = cli('status')
        assert not after['flags'] & 8, 'unsaved changes left behind'
        print('PASS restored: layout, loop rates, no unsaved changes', flush=True)
    finally:
        try:
            if directory.exists():
                uninstall(directory)
                print('PASS uninstalled', flush=True)
        finally:
            if device is not None:
                device.kill()
                device.communicate(timeout=10)
            shutil.rmtree(work, ignore_errors=True)
    print('PASS installed GUI smoke', flush=True)


if __name__ == '__main__':
    main()
