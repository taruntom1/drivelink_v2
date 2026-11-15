"""Deterministic wire replay for CLI event, action, capture and CSV goldens.

The fixtures were recorded with the unmodified 2793125 CLI. The synthetic
descriptor is served by the peer; it is not a module definition in host code.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import signal
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware/test/uart_comm'))
from protocol import encode_frame


def pack(fmt, *args):
    return struct.pack('<' + fmt, *args)


def string(value):
    data = value.encode()
    return pack('H', len(data)) + data


def exact(peer, count):
    data = bytearray()
    while len(data) < count:
        chunk = peer.recv(count - len(data))
        if not chunk:
            raise EOFError()
        data.extend(chunk)
    return bytes(data)


INFO = pack('B', 9) + string('replay') + string('0123456789abcdef') + pack('IIHBBB', 123, 4096, 1024, 2, 0, 0) + bytes.fromhex('02444c00000100')
REF = pack('BBH', 112, 0, 1)
CAPTURE = (pack('BBBHIIB', 3, 1, 0, 1, 2, 1, 0) + bytes(4) + pack('BIIIIQB', 0, 0, 2, 1, 1000, 42, 1) + REF)
EVENTS = [
    (128, pack('BBHQBBBBBB', 9, 7, 65535, 123456, 1, 3, 2, 11, 22, 2) + pack('BIBIB', 3, 5, 19, 9, 4)),
    (193, pack('BBIIB', 9, 1, 2, 1, 2)),
    (193, pack('BB BIB', 9, 2, 3, 99, 2)),
    (192, pack('BHBBQBBBHiII', 3, 65535, 1, 0, 123, 2, 112, 7, 42, -1, 99, 101)),
    (192, pack('BHBBQB BBHII', 3, 2, 2, 0, 124, 1, 112, 7, 42, 3, 9)),
    (192, pack('BHBBQBBH', 3, 3, 3, 0, 125, 112, 7, 42)),
    (192, pack('BHBBQBBBB', 3, 4, 4, 0, 126, 1, 2, 0, 1)),
    (224, b'\x01\xab'),
]


def descriptor(action):
    data = pack('HBBBBIII', 1, 3 if action else 1, 255 if action else 5, 0, 0, 0, 0, 0)
    data += b''.join(string(s) for s in ('sample', 'rad', 'Replay item', ''))
    data += pack('BBBB', 1 if action else 0, 3 if action else 0, 1 if action else 0, 1 if action else 0)
    if action:
        data += pack('B', 1) + string('argument') + string('')
        data += pack('B', 4) + string('result') + string('')
    return pack('HHB', 1, 65535, 1) + data


def run_case(cli, command, json_mode):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(10)
    errors = []
    process = None
    child_ready = threading.Event()
    disarmed = threading.Event()

    def serve():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(10)
                while True:
                    header = exact(peer, 7)
                    data = exact(peer, int.from_bytes(header[4:6], 'little') + 2)[:-2]
                    if 160 <= header[3] <= 191:
                        continue  # unacknowledged keepalive before START
                    family, op = data[2:4]
                    if (family, op) == (1, 1):
                        body = INFO
                    elif (family, op) == (6, 4):
                        body = CAPTURE
                    elif (family, op) == (6, 5):
                        body = pack('IHII', 0, 2, 0x3fc00000, 0xc0000000)
                    elif (family, op) == (3, 2):
                        body = descriptor(command[0] == 'action')
                    elif (family, op) == (8, 1):
                        body = pack('IIIIH', 0xffffffff, 0, 0, 0, 2) + b'\xab\xcd'
                    elif (family, op) == (4, 1):
                        body = pack('BI', 1, 1000) if data[4] == 1 else pack('BIII', 3, 0 if command[0] == 'testgen-run' else 1,
                                                                                      1 if command[0] == 'testgen-run' else 0, 64)
                    elif (family, op) == (5, 6):
                        if command[0] == 'testgen-run':
                            body = pack('HHB BBBBBHB', 1, 65535, 1, 1, 0, 1, 0, 4, 1, 0)
                        else:
                            body = pack('HHB BBBBBHB', 1, 65535, 1, 0, 0, 3, 0, 4, 1, 1) + REF
                    elif (family, op) in ((1, 2), (1, 3)):
                        body = b''
                        if op == 3:
                            disarmed.set()
                    elif (family, op) == (7, 1):
                        # START is in flight, and no STATUS has been read.
                        child_ready.wait(timeout=5)
                        os.kill(process.pid, signal.SIGINT)
                        time.sleep(.05)
                        body = pack('I', 99)
                    else:
                        raise AssertionError((family, op, data.hex()))
                    peer.sendall(encode_frame(65, bytes([9, data[1], family, op, 0, 0]) + body))
                    if (family, op) == (1, 1) and command[0] in ('events', 'status'):
                        for kind, event in (EVENTS if command[0] == 'events' else EVENTS[:1]):
                            peer.sendall(encode_frame(kind, event))
                    if (family, op) == (3, 2) and command[0] in ('sub-stream', 'sub-csv'):
                        peer.sendall(encode_frame(129, pack('BBHQI', 9, 4, 65535, 77, 0x3fc00000)))
        except (EOFError, ConnectionResetError):
            pass
        except Exception as error:
            errors.append(error)
        finally:
            listener.close()

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        process = subprocess.Popen([cli, '--endpoint', f'tcp://127.0.0.1:{listener.getsockname()[1]}',
                                    *(['--json'] if json_mode else []), *command],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        child_ready.set()
        stdout, stderr = process.communicate(timeout=15)
        result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
        worker.join(timeout=11)
        listener.close()
    assert not worker.is_alive(), 'replay worker did not stop'
    assert not errors, errors
    if command[0] == 'testgen-run':
        assert disarmed.is_set(), 'interrupted START did not send DISARM'
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    output = [json.loads(line) for line in result.stdout.splitlines()] if json_mode else result.stdout
    return dict(command=command, json=json_mode, stdout=output, stderr=result.stderr, code=result.returncode)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--record', action='store_true')
    args = ap.parse_args()
    actual = []
    for mode in (False, True):
        for command in (['events', '8'], ['status'], ['capture-read'], ['action', '112:0:1', '7', '--blob', '12'], ['sub-stream', '0']):
            actual.append(run_case(args.cli, command, mode))
    for command in (['capture-read', '--csv'], ['sub-csv', '0', '1']):
        actual.append(run_case(args.cli, command, False))
    fixture = Path(__file__).with_name('fixtures') / 'cli-replay.json'
    if args.record:
        fixture.write_text(json.dumps(actual, indent=2) + '\n', encoding='utf-8')
    else:
        expected = json.loads(fixture.read_text(encoding='utf-8'))
        for before, after in zip(expected, actual, strict=True):
            assert before == after, (before, after)
    if os.name != 'nt':
        # POSIX allows deterministic SIGINT to just this process. Assert the
        # established pre-refactor null-status contract and successful DISARM.
        interrupted = run_case(args.cli, ['testgen-run', '0', '100', '1000', '0', '112:0:1',
                                         'step', '0', '.1', '0', '0', '1000', '1'], True)
        assert interrupted['stdout'] == [{'ok': True, 'data': {'status': None, 'interrupted': True}}], interrupted
    print(f'PASS {len(actual)} deterministic CLI replay outputs (events, action, capture, subscription and CSV)')


if __name__ == '__main__':
    main()
