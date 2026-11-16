"""Render real GUI shell scenarios; optionally repeat the vdev safety checks.

All provisioning and fault injection targets an isolated vdev. Use the app's
--endpoint/--out options directly for an existing physical device.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading


def render(gui, vdev, destination, scenario):
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
            screen = {'safe': 'device', 'fault': 'drawer', 'lost': 'device'}.get(scenario, scenario)
            out = destination / scenario
            command = [gui, '--screen', screen, '--out', str(out)]
            if endpoint:
                command += ['--endpoint', endpoint]
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
                    if line.strip() == 'GUI READY' and scenario in ('fault', 'lost'):
                        next_file = Path(work) / 'next'
                        next_file.write_text('1 ' + ('fault' if scenario == 'fault' else 'stall') + '\n')
                        os.replace(next_file, control)
                code = app.wait(timeout=5)
            finally:
                timer.cancel()
                if app.poll() is None:
                    app.kill()
                    app.wait(timeout=5)
                thread.join(timeout=5)
            assert code == 0, (scenario, output, errors)
            images = sorted(out.glob('*.png'))
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
    parser.add_argument('--out', required=True)
    parser.add_argument('--smoke', action='store_true', help='Only Connect and device shell')
    args = parser.parse_args()
    os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
    os.environ.setdefault('QT_QUICK_BACKEND', 'software')
    os.environ.setdefault('QT_FORCE_STDERR_LOGGING', '1')
    if args.backend:
        subprocess.run([args.backend], check=True, timeout=180)
    destination = Path(args.out).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    for scenario in (['connect', 'device'] if args.smoke else
                     ['connect', 'device', 'layout', 'tune', 'monitor', 'drawer', 'dialog', 'safe', 'fault', 'lost']):
        render(args.gui, args.vdev, destination, scenario)


if __name__ == '__main__':
    main()
