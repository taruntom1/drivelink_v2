"""Drive the shipped CLI against a separate vdev process; no hardware needed."""
import argparse
import binascii
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
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
            # Native loops can own a slot at the publish attempt. Retry only
            # explicit no-effect staging rejections in this test harness.
            # Production CLI/Session never retries a mutation or its timeout.
            from motor_smoke import retryable_rejection
            for attempt in range(10):
                completed = subprocess.run([args.cli, '--endpoint', endpoint or url, '--json', *command],
                                           capture_output=True, text=True, timeout=20)
                result = json.loads(completed.stdout)
                if code == 0 and completed.returncode == 5 and retryable_rejection(command,result) and attempt < 9:
                    time.sleep(.02)
                    continue
                break
            assert completed.returncode == code, (command, completed.returncode, completed.stdout, completed.stderr)
            assert result['ok'] == (code == 0), result
            if command == ('reboot',) and code == 0:
                # REBOOT acknowledges acceptance before Boot is delivered. A
                # PTY stays open, so Boot can invalidate the next CLI's initial
                # GET_INFO. Poll only this read; never repeat the mutation or
                # swallow other protocol errors/failed scenario assertions.
                for ready_attempt in range(10):
                    ready = subprocess.run([args.cli, '--endpoint', endpoint or url, '--json', 'info'],
                                           capture_output=True, text=True, timeout=20)
                    data = json.loads(ready.stdout)
                    if ready.returncode == 0 and data['ok']:
                        break
                    transient = ready.returncode in (3, 4) or (
                        ready.returncode == 6 and data.get('error') == 'device reboot during request')
                    assert transient and ready_attempt < 9, (ready.returncode, data, ready.stderr)
                    time.sleep(.02)
            return result.get('data', result)

        try:
            url = start()
            assert cli('info')['max_payload'] == 1024
            ping=cli('time-ping')['samples'][0]
            assert ping['accepted'] and ping['esp_tx_us'] >= ping['esp_rx_us']
            human_time = subprocess.run([args.cli, '--endpoint', url, 'time-ping'],
                                        capture_output=True, text=True, timeout=10, check=True)
            assert 'samples' in human_time.stdout and 'mapped_' not in human_time.stdout and 'mapping_' not in human_time.stdout
            assert len(cli('time-sync','1')['samples']) >= 3
            sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'firmware/test/uart_comm'))
            from protocol import encode_frame, UartCommProto
            # Invalid pings are dropped; the first time reply must be the valid
            # one, and scanning its CRC proves the queued TX patch was finalized.
            address,port=url[6:].rsplit(':',1)
            with socket.create_connection((address,int(port)), timeout=3) as raw:
                raw.sendall(encode_frame(2,b'\x01')+encode_frame(2,struct.pack('<BH',2,77))+
                            encode_frame(2,struct.pack('<BH',1,78)))
                class Wire:
                    in_waiting=0
                    def read(self, n):
                        return raw.recv(n)
                decoder=UartCommProto(Wire())
                deadline=time.monotonic()+3
                while time.monotonic()<deadline:
                    kind,payload=decoder.read_frame(max(.01,deadline-time.monotonic()))
                    if kind==0x42:
                        version,ident,rx,tx=struct.unpack('<BHQQ',payload)
                        assert (version,ident)==(1,78) and tx>=rx
                        break
                else:
                    raise AssertionError('no valid time reply after invalid requests')
            rejected=next(e for e in cli('diag-entries') if e['source']==4 and e['code']==0x106)
            assert rejected['count']>=2, rejected
            time_check = subprocess.run([sys.executable, str(Path(__file__).with_name('time_smoke.py')),
                '--cli', args.cli, '--endpoint', url, '--duration', '1', '--warmup-only', '--traffic',
                '--report', str(Path(state)/'time-dry-run.json')], capture_output=True, text=True, timeout=25)
            assert time_check.returncode == 0, (time_check.stdout, time_check.stderr)
            print(time_check.stdout, end='')
            assert len(cli('types')) >= 4
            for type_ in ('0', '112', '113', '114'):
                assert cli('items', type_)
            assert cli('codes')
            assert len(cli('instances')) == 2
            motor = cli('add', '112', 'motor')['instance']
            encoder = cli('add', '113', 'encoder')['instance']
            imu = cli('add', '114', 'imu')['instance']
            assert len(cli('instances', 'pending')) == 5
            wiring = (112 << 8) | motor
            assert cli('set', f'113:{encoder}:1={wiring}')['pending_reboot']
            cli('save'); cli('reboot')
            assert len(cli('instances')) == 5
            assert cli('get', '5:0:1')['typed_values'] == [1000]
            assert cli('set', '5:0:1=500')['typed_values'] == [500]
            cli('save')
            result = cli('action', f'112:{motor}:768', '0.5')
            time.sleep(.1)
            assert cli('action', f'112:{motor}:768', '0.5')['typed_values'][0] == 0
            assert cli('action', f'112:{motor}:769', '--blob', '616263')['blob_hex'] == '616263'
            assert cli('status')['msg_type'] == 128
            # Groups persist across sessions, but never across device reboots.
            assert cli('groups')['subscription_capacity'] == 8
            sub = cli('sub-config', '0', '0', '2', f'112:{motor}:256', f'112:{motor}:512')
            assert sub == {'epoch': 1, 'channel': 129}
            sample = cli('sub-stream', '0')
            assert len(sample['typed_values']) == 2 and sample['epoch'] == 1
            cli('sub-config', '1', '1', '1', f'114:{imu}:256')
            assert cli('sub-stream', '1')['typed_values'][0] != 0
            cli('sub-config', '2', '0', '1', '5:1:256', code=5)
            cli('cmd-config', '0', '0', f'112:{motor}:256', code=5)  # signals cannot be commanded
            command = cli('cmd-config', '0', '0', f'112:{motor}:512')
            assert command == {'epoch': 1, 'channel': 160}
            cli('cmd-config', '1', '0', f'112:{motor}:512', code=5)  # one writer per input
            cli('cmd-send', '0', '65535', '1.0')
            cli('cmd-send', '0', '0')  # keepalive
            cli('cmd-send', '0', '1', 'NaN', code=2)
            # F4a never arms: the command above must not alter the mock input.
            assert cli('sub-stream', '0')['typed_values'][1] == 0.0
            assert cli('get', '0:0:16', '0:0:17', '0:0:18', '0:0:19', '0:0:20')['typed_values'] == [4,16,8,8,64]
            assert cli('set', '0:0:16=1', code=5)['detail'] == 7
            # Desktop scheduling needs a wider stall window than a 1 kHz board.
            cli('set', '5:0:1=100', '0:0:4=1000')
            assert cli('cmd-run', '0', '20', '8', '0.6')['sent'] == 8
            assert cli('status')['arm_state'] == 0
            assert cli('sub-stream', '0')['typed_values'][0] > 0
            assert cli('keepalive', '0', '20', '2')['sent'] == 2
            csv = subprocess.run([args.cli, '--endpoint', url, 'sub-csv', '0', '3'], capture_output=True, text=True, timeout=10)
            assert csv.returncode == 0 and csv.stdout.splitlines()[0].startswith('timestamp_us,counter,112:')
            assert len(csv.stdout.splitlines()) == 4
            cli('action', f'112:{motor}:770', 'false', 'true')
            assert cli('arm', code=5)['detail'] == 39
            cli('action', f'112:{motor}:770', 'false', 'false')
            cli('arm')
            cli('save', code=5); cli('cmd-clear', '0', code=5)
            cli('set', '0:0:4=50', code=5)
            cli('action', f'112:{motor}:769', code=5)
            cli('action', f'112:{motor}:770', 'true', 'true')
            assert cli('status')['arm_state'] == 2
            assert cli('arm', code=5)['detail'] == 38
            assert cli('ack-fault', code=5)['detail'] == 43
            cli('diag-clear')
            assert cli('ack-fault', code=5)['detail'] == 43
            cli('action', f'112:{motor}:770', 'true', 'false')
            assert cli('arm', code=5)['detail'] == 38
            cli('ack-fault'); cli('arm'); cli('disarm')
            assert cli('status')['arm_state'] == 0
            cli('set', '0:0:4=50'); cli('arm'); time.sleep(.15)
            arm_status = cli('status')
            assert arm_status['arm_state'] == 0 and arm_status['last_disarm_reason'] == 2
            cli('arm'); cli('disarm')  # no ACK is needed after the watchdog
            cli('set', '5:0:1=500', '0:0:4=200')
            cli('bench-stall', '0', '10', code=5)
            assert cli('sub-enable', '0', 'false')['epoch'] == 2
            assert cli('sub-enable', '0', 'true')['epoch'] == 3
            assert cli('sub-stream', '0')['epoch'] == 3
            cli('sub-clear', '0'); cli('sub-clear', '1'); cli('cmd-clear', '0')
            cli('diag-info'); cli('diag-entries'); cli('diag-clear')
            cli('set', '5:0:1=NaN', code=2)
            cli('bench-crash', code=5)
            human = subprocess.run([args.cli, '--endpoint', url, 'info'], capture_output=True, text=True, timeout=20)
            assert human.returncode == 0 and 'protocol_version: 9' in human.stdout
            kill()
            # External process restarts load config, not just Core reconstruction.
            url = start()
            assert len(cli('instances')) == 5
            assert cli('get', '5:0:1')['typed_values'] == [500]
            assert all(not group['configured'] for group in cli('groups')['groups'])
            kill()
            # Commit one new-generation instance blob, then interrupt SAVE.
            # Reopening must still load the previous complete generation.
            url = start('--fail-writes-after', '1')
            cli('set', '5:0:1=600'); cli('save', code=5)
            kill()
            # Three consecutive unclean exits enter safe mode. Nothing is created.
            url = start()
            assert cli('info')['flags'] & 8
            assert cli('info')['running_instances'] == 0
            assert cli('info')['loops'] == []
            assert cli('status')['overruns'] == []
            assert len(cli('instances')) == 5
            assert cli('get', '5:0:1')['typed_values'] == [500]
            cli('exit-safe-mode')
            assert cli('info')['running_instances'] == 5
            assert not cli('info')['flags'] & 8
            cli('remove', '113', str(encoder)); cli('remove', '112', str(motor)); cli('remove', '114', str(imu))
            cli('save'); cli('reboot'); assert len(cli('instances')) == 2
            # A second process cannot share the persistent state directory.
            other = subprocess.run([args.vdev, '--state', state, '--port', '0'], capture_output=True, text=True, timeout=10)
            assert other.returncode != 0 and 'already in use' in other.stderr
            if os.name != 'nt':
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=10)
                assert process.returncode == 0
                process = None
                url = start('--pty')
                assert cli('info')['protocol_version'] == 9
                assert cli('time-ping')['samples'][0]['accepted']
                assert cli('diag-info')['reset_reason'] == 0
                cli('set', '0:0:3=100')
                # More than the PTY's kernel buffer: no external reader must
                # not permanently destroy the master or its published path.
                time.sleep(15)
                # Submit REBOOT without draining the filled slave. Its reply
                # deadline must cancel the effect and recover the same PTY,
                # rather than close the published path or run a stale handler.
                payload = bytes((9, 201, 1, 8))
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
                assert cli('info')['protocol_version'] == 9
                assert cli('get', '0:0:3')['typed_values'] == [100], 'undrained REBOOT must be cancelled'
                cli('save'); cli('reboot'); assert len(cli('instances')) == 2
            # A loop is an ordinary instance; its id is preserved on the wire
            # and across both protocol reboots and external process restarts.
            extra = cli('add', '5', 'third')['instance']
            assert cli('set', f'5:{extra}:2=true', f'5:{extra}:3=1')['pending_reboot']
            assert cli('set', f'5:{extra}:1=250')['pending_reboot']
            motor = cli('add', '112', 'third_motor', str(extra))['instance']
            cli('remove', '5', str(extra), code=5)
            cli('save'); cli('reboot')
            loops = cli('info')['loops']
            assert len(loops) == 3 and next(l for l in loops if l['id'] == extra)['realtime']
            assert {e['loop_id'] for e in cli('status')['overruns']} == {0, 1, extra}
            assert cli('get', f'5:{extra}:1')['typed_values'] == [250]
            cli('set', f'5:{extra}:1=400'); cli('save'); cli('reboot')
            assert cli('get', f'5:{extra}:1')['typed_values'] == [400]
            cli('remove', '112', str(motor)); cli('remove', '5', str(extra))
            cli('save'); cli('reboot'); assert len(cli('instances')) == 2
            # Source params and their generated signals are discovered through
            # ordinary item descriptors; both same-loop and cross-loop samples
            # stream through the existing subscription family.
            items = {item['name']: item for item in cli('items', '115')}
            assert items['feedback']['is_source'] and items['feedback']['flags'] & 128
            assert items['feedback.value']['value_type'] == 5
            assert items['feedback.age_us']['value_type'] == 3
            assert cli('get', '0:0:21')['typed_values'] == [32]
            assert cli('set', '0:0:21=1', code=5)['detail'] == 7
            feedback = cli('add', '113', 'source_feedback', '0')['instance']
            imu = cli('add', '114', 'source_imu', '1')['instance']
            fusion = cli('add', '115', 'fusion', '0')['instance']
            feedback_ref = (113 << 24) | (feedback << 16) | 256
            imu_ref = (114 << 24) | (imu << 16) | 256
            assert cli('set', f'115:{fusion}:1={feedback_ref}', f'115:{fusion}:2={imu_ref}')['pending_reboot']
            cli('remove', '114', str(imu), code=5)
            cli('save'); cli('reboot')
            assert cli('get', f'115:{fusion}:1', f'115:{fusion}:2')['typed_values'] == [feedback_ref, imu_ref]
            cli('sub-config', '0', '0', '10', *(f'115:{fusion}:{i}' for i in range(256,261)))
            sample = cli('sub-stream', '0')['typed_values']
            assert sample[1] == 0 and 0 <= sample[3] < 4294967295
            assert abs(sample[4] - (sample[0]+sample[2])) < 1e-5
            cli('cmd-config', '0', '0')
            cli('arm'); cli('disarm')
            assert cli('sub-stream', '0')['typed_values'][3] < 4294967295
            cli('cmd-clear', '0')
            cli('sub-clear', '0')
            cli('set', f'115:{fusion}:2=0'); cli('save'); cli('reboot')
            cli('sub-config', '0', '0', '10', f'115:{fusion}:259')
            assert cli('sub-stream', '0')['typed_values'] == [4294967295]
            cli('sub-clear', '0'); cli('remove', '115', str(fusion))
            cli('remove', '113', str(feedback)); cli('remove', '114', str(imu))
            cli('save'); cli('reboot'); assert len(cli('instances')) == 2
            from motor_smoke import run as motor_smoke
            from loop_affinity_smoke import run as loop_affinity_smoke
            loop_affinity_smoke(args.cli, url)
            from imu_smoke import run as imu_smoke
            imu_smoke(args.cli, url)
            from capture_smoke import run as capture_smoke
            capture_smoke(args.cli, url, loop_rate=200)
            from testgen_smoke import run as testgen_smoke
            testgen_smoke(args.cli, url, loop_rate=50, flash_write=True)
            from ota_smoke import simulated as ota_smoke
            ota_smoke(args.cli, url)
            # The loop-stall scenario needs the bench stall: restart the same
            # state with --allow-crash, on the same transport.
            kill()
            url = start('--allow-crash', *(('--pty',) if url.startswith('serial://') else ()))
            motor_smoke(args.cli, url, stall_ms=2000)
            # The vdev has no pins: the real-module path refuses before any
            # change, whether the endpoint is TCP or (on Linux) a serial PTY.
            refused = subprocess.run([sys.executable, str(Path(__file__).with_name('motor_smoke.py')), '--cli', str(args.cli),
                                      '--endpoint', url, '--modules', 'real', '--no-motor', '--pwm-pin', '4',
                                      '--dir-pin', '5', '--enc-a', '6', '--enc-b', '7'],
                                     capture_output=True, text=True, timeout=30)
            assert refused.returncode in (1, 2) and 'no configuration was changed' in refused.stderr, refused
            assert 'Traceback' not in refused.stderr and 'PASS' not in refused.stdout, refused
            assert cli('types') and len(cli('instances')) == 2
            print('PASS CLI discovery, typed params, actions, status, diag, persistence, failed SAVE, crash recovery, locking and transports')
        finally:
            kill()


if __name__ == '__main__':
    main()
