"""H1c identity workflow on vdev or a bare board, with restored name/layout.

Requires saved clean settings, idle groups and a disarmed normal device.
Optional async_probe checks rename while armed with a temporary mock actuator.
"""
import argparse
import json
import subprocess
import tempfile
import time


def run(cli, endpoint, probe=None, expected_id=None):
    def command(*args, code=0):
        result = subprocess.run([cli, '--endpoint', endpoint, '--json', *args],
                                capture_output=True, encoding='utf-8', timeout=30)
        assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
        data = json.loads(result.stdout)
        assert data['ok'] == (code == 0), data
        return data.get('data', data)

    def connected():
        deadline = time.monotonic() + 20
        while True:
            try:
                return command('info')
            except AssertionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.2)

    def reboot():
        command('reboot')
        return connected()

    def dirty(expected):
        # CH343 can deliver queued status frames after a fresh command reply.
        # Poll state, without treating host delivery time as device timing.
        deadline = time.monotonic() + 10
        while True:
            status = command('status')
            if bool(status['flags'] & 8) == expected:
                return
            assert time.monotonic() < deadline, ('dirty state did not arrive', expected, status)
            time.sleep(.1)

    original = connected()
    assert original['protocol_version'] == 12 and len(original['device_id']) == 12
    if expected_id:
        assert original['device_id'] == expected_id
    assert not original['flags'] & 11, 'requires clean valid normal layout'
    assert not command('status')['flags'] & 8, 'requires saved settings'
    assert command('status')['arm_state'] == 0, 'requires disarmed device'
    assert all(not g['configured'] for g in command('groups')['groups']), 'requires idle groups'
    layout = command('instances')
    mock = None
    try:
        for name in ('sixteenbytes1234', 'base \u00e9', '\U0001f697' * 4, ''):
            assert command('name', name)['name'] == name
            current = command('info')
            assert current['device_id'] == original['device_id'] and current['name'] == name
            dirty(True)
        command('name', 'saved identity'); command('save')
        dirty(False)
        assert reboot()['name'] == 'saved identity'
        command('name', 'unsaved')
        assert reboot()['name'] == 'saved identity'
        command('name', 'revert me'); command('revert')
        assert command('name')['name'] == 'saved identity'
        command('name', 'x' * 17, code=2)
        command('name', 'bad\nname', code=2)
        assert command('name')['name'] == 'saved identity'
        if probe:
            mock = command('add', '112', 'identity_mock')['instance']
            command('save'); reboot()
            result = subprocess.run([probe, endpoint], capture_output=True, encoding='utf-8', timeout=45)
            assert result.returncode == 0, (result.stdout, result.stderr)
            print(result.stdout, end='')
        print('PASS identity, byte limits, Unicode, SAVE/reboot, unsaved reboot and REVERT', flush=True)
    finally:
        command('disarm')
        if mock is not None:
            command('remove', '112', str(mock))
        command('name', original['name']); command('save')
        restored = reboot()
        assert command('instances') == layout
        assert restored['name'] == original['name'] and restored['device_id'] == original['device_id']
        dirty(False)
        assert command('status')['arm_state'] == 0
        assert all(not g['configured'] for g in command('groups')['groups'])
        print('PASS original identity/layout restored, disarmed with idle groups', flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint')
    ap.add_argument('--vdev')
    ap.add_argument('--probe')
    args = ap.parse_args()
    assert bool(args.endpoint) != bool(args.vdev), 'supply --endpoint or --vdev'
    if args.endpoint:
        run(args.cli, args.endpoint, args.probe)
    else:
        with tempfile.TemporaryDirectory(prefix='drivelink-identity-') as state:
            process = subprocess.Popen([args.vdev, '--port', '0', '--state', state,
                                        '--device-id', '02444c010203'], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, encoding='utf-8')
            try:
                ready = process.stdout.readline().strip()
                assert ready.startswith('READY '), ready
                run(args.cli, ready[6:], args.probe, '02444c010203')
            finally:
                process.kill(); process.communicate(timeout=10)


if __name__ == '__main__':
    main()
