"""Drive the shipped CLI against a separate vdev process; no hardware needed."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--vdev', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='drivelink-integration-') as state:
        process = None

        def start(*extra):
            nonlocal process
            process = subprocess.Popen([args.vdev, '--port', '0', '--state', state, *extra],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            line = process.stdout.readline().strip()
            assert line.startswith('READY '), (line, process.stderr.read())
            return line[6:]

        def kill():
            nonlocal process
            if process is not None:
                process.kill()
                process.communicate(timeout=10)
                process = None

        def cli(*command, code=0, endpoint=None):
            # Native loop threads are not real-time. Withdrawn guarantees no
            # effect, so this harness can repeat it. Production CLI/Session
            # never retries a mutation or its timeout.
            for attempt in range(10):
                completed = subprocess.run([args.cli, '--endpoint', endpoint or url, '--json', *command],
                                           capture_output=True, text=True, timeout=20)
                result = json.loads(completed.stdout)
                if code == 0 and completed.returncode == 5 and result.get('result') == 5 and result.get('detail') == 15 and attempt < 9:
                    time.sleep(.02)
                    continue
                break
            assert completed.returncode == code, (command, completed.returncode, completed.stdout, completed.stderr)
            assert result['ok'] == (code == 0), result
            return result.get('data', result)

        try:
            url = start()
            assert cli('info')['max_payload'] == 1024
            assert len(cli('types')) >= 4
            for type_ in ('0', '112', '113', '114'):
                assert cli('items', type_)
            assert cli('codes')
            assert cli('instances') == []
            motor = cli('add', '112', 'motor')['instance']
            encoder = cli('add', '113', 'encoder')['instance']
            imu = cli('add', '114', 'imu')['instance']
            assert len(cli('instances', 'pending')) == 3
            wiring = (112 << 8) | motor
            assert cli('set', f'113:{encoder}:1={wiring}')['pending_reboot']
            cli('save'); cli('reboot')
            assert len(cli('instances')) == 3
            assert cli('get', '0:0:1')['typed_values'] == [1000]
            assert cli('set', '0:0:1=500')['typed_values'] == [500]
            cli('save')
            result = cli('action', f'112:{motor}:768', '0.5')
            time.sleep(.1)
            assert cli('action', f'112:{motor}:768', '0.5')['typed_values'][0] > 0
            assert cli('action', f'112:{motor}:769', '--blob', '616263')['blob_hex'] == '616263'
            assert cli('status')['msg_type'] == 128
            cli('diag-info'); cli('diag-entries'); cli('diag-clear')
            cli('set', '0:0:1=NaN', code=2)
            cli('bench-crash', code=5)
            human = subprocess.run([args.cli, '--endpoint', url, 'info'], capture_output=True, text=True, timeout=20)
            assert human.returncode == 0 and 'protocol_version: 1' in human.stdout
            kill()
            # External process restarts load config, not just Core reconstruction.
            url = start()
            assert len(cli('instances')) == 3
            assert cli('get', '0:0:1')['typed_values'] == [500]
            kill()
            url = start('--fail-writes-after', '0')
            cli('set', '0:0:1=600'); cli('save', code=5)
            kill()
            # Three consecutive unclean exits enter safe mode. Nothing is created.
            url = start()
            assert cli('info')['flags'] & 8
            assert cli('info')['running_instances'] == 0
            assert len(cli('instances')) == 3
            assert cli('get', '0:0:1')['typed_values'] == [500]
            cli('exit-safe-mode')
            assert cli('info')['running_instances'] == 3
            assert not cli('info')['flags'] & 8
            cli('remove', '113', str(encoder)); cli('remove', '112', str(motor)); cli('remove', '114', str(imu))
            cli('save'); cli('reboot'); assert cli('instances') == []
            # A second process cannot share the persistent state directory.
            other = subprocess.run([args.vdev, '--state', state, '--port', '0'], capture_output=True, text=True, timeout=10)
            assert other.returncode != 0 and 'already in use' in other.stderr
            if os.name != 'nt':
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=10)
                assert process.returncode == 0
                process = None
                url = start('--pty')
                assert cli('info')['protocol_version'] == 1
                cli('save'); cli('reboot'); assert cli('instances') == []
            print('PASS CLI discovery, typed params, actions, status, diag, persistence, failed SAVE, crash recovery, locking and transports')
        finally:
            kill()


if __name__ == '__main__':
    main()
