"""Drive the shipped CLI against a separate vdev process; no hardware needed."""
import argparse
import binascii
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
            # Commit one new-generation instance blob, then interrupt SAVE.
            # Reopening must still load the previous complete generation.
            url = start('--fail-writes-after', '1')
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
                assert cli('diag-info')['reset_reason'] == 0
                cli('set', '0:0:3=100')
                # More than the PTY's kernel buffer: no external reader must
                # not permanently destroy the master or its published path.
                time.sleep(15)
                # Submit REBOOT without draining the filled slave. Its reply
                # deadline must cancel the effect and recover the same PTY,
                # rather than close the published path or run a stale handler.
                payload = bytes((1, 201, 1, 8))
                header = bytes((1, len(payload), 0))
                crc = 0
                for byte in header:
                    crc ^= byte
                    for _ in range(8):
                        crc = ((crc << 1) ^ (0x07 if crc & 0x80 else 0)) & 255
                body = header + bytes((crc,)) + payload
                frame = b'\xaa\xaa\xaa' + body + binascii.crc_hqx(body, 0xffff).to_bytes(2, 'little')
                fd = os.open(url[9:], os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
                try:
                    assert os.write(fd, frame) == len(frame)
                    time.sleep(3)
                finally:
                    os.close(fd)
                assert cli('info')['protocol_version'] == 1
                assert cli('get', '0:0:3')['typed_values'] == [100], 'undrained REBOOT must be cancelled'
                cli('save'); cli('reboot'); assert cli('instances') == []
            print('PASS CLI discovery, typed params, actions, status, diag, persistence, failed SAVE, crash recovery, locking and transports')
        finally:
            kill()


if __name__ == '__main__':
    main()
