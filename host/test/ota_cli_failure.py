"""OTA workflow regressions through a real vdev frame proxy (TCP only)."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import threading
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware/test/uart_comm'))
from protocol import UartCommProto, encode_frame, CORE_PROTOCOL_VERSION


def check(cli_path, endpoint, image, mismatch=False, hold_commit=False):
    remote = urlparse(endpoint)
    if remote.scheme != 'tcp':
        return  # TCP case runs on both OSes; PTY still runs the shared campaign.
    stopped = threading.Event()
    listener = socket.socket(); listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(.1)
    endpoint = f'tcp://127.0.0.1:{listener.getsockname()[1]}'
    seen = {'confirm': False, 'mismatch': False, 'reboot': 0}
    client_write = threading.Lock()
    dropped = set()
    workers, sockets, errors = [], [], []

    def pump(source, destination, incoming, connection_stopped):
        buffered = bytearray()
        try:
            while not stopped.is_set():
                try:
                    chunk = source.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buffered.extend(chunk)
                while (frame := UartCommProto._scan(buffered)) is not None:
                    kind, body = frame
                    if not incoming and kind == 1 and body[2:4] == bytes((1, 8)):
                        seen['reboot'] += 1
                        if hold_commit:
                            # Model accepted REBOOT followed by TX-drain/commit
                            # cancellation: reply succeeds, device stays Ready.
                            with client_write:
                                source.sendall(encode_frame(0x41, bytes((body[0], body[1], 1, 8, 0, 0))))
                            continue
                    if not incoming and kind == 1 and body[2:4] == bytes((9, 5)):
                        seen['confirm'] = True
                    if incoming and kind == 0x41 and len(body) >= 6 and body[2] == 9 and body[4:6] == bytes((0, 0)):
                        if mismatch and body[3] == 6 and len(body) == 94 and body[9] == 1:
                            body = bytearray(body); body[62] ^= 1; seen['mismatch'] = True
                        elif not mismatch and not hold_commit and body[3] in (1, 2, 3, 5) and body[3] not in dropped:
                            dropped.add(body[3]); continue
                    if incoming:
                        with client_write:
                            destination.sendall(encode_frame(kind, body))
                    else:
                        destination.sendall(encode_frame(kind, body))
        except (ConnectionResetError, BrokenPipeError):
            pass
        except OSError as error:
            if not stopped.is_set() and not connection_stopped.is_set():
                errors.append(str(error))
        finally:
            connection_stopped.set()
            # Close this connection only. The CLI reconnects through the same
            # listener after SYSTEM REBOOT, with no proxy-induced extra reboot.
            for current in (source, destination):
                try:
                    current.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def accept():
        while not stopped.is_set():
            try:
                client = listener.accept()[0]
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                device = socket.create_connection((remote.hostname, remote.port), timeout=3)
                client.settimeout(.1); device.settimeout(.1); sockets.extend((client, device))
                connection_stopped = threading.Event()
                for source, destination, incoming in ((client, device, False), (device, client, True)):
                    thread = threading.Thread(target=pump, args=(source, destination, incoming, connection_stopped)); workers.append(thread); thread.start()
            except OSError as error:
                errors.append(str(error)); client.close()

    acceptor = threading.Thread(target=accept); acceptor.start()
    try:
        result = subprocess.run([str(cli_path), '--endpoint', endpoint, '--timeout-ms', '300', '--json',
                                 'ota-upload', str(image)], capture_output=True, text=True, timeout=45)
    finally:
        stopped.set(); listener.close(); acceptor.join(5)
        for current in sockets:
            current.close()
        for thread in workers:
            thread.join(5)
    assert not acceptor.is_alive() and all(not x.is_alive() for x in workers) and not errors, errors
    data = json.loads(result.stdout)
    if hold_commit:
        assert result.returncode == 6 and not data['ok'] and seen['reboot'] == 1 and not seen['confirm'], (data, seen, result.stderr)
        assert 'commit did not complete' in data['error'], data
        print('PASS accepted but uncommitted OTA reboot is reported without an automatic second reboot', flush=True)
    elif mismatch:
        assert result.returncode == 6 and not data['ok'] and seen['mismatch'] and not seen['confirm'], (data, seen, result.stderr)
        print('PASS OTA refuses confirmation when reconnected build identity mismatches', flush=True)
    else:
        assert result.returncode == 0 and data['ok'] and data['data']['confirmed'], (data, dropped, result.stderr)
        assert dropped == {1, 2, 3, 5} and seen['confirm'], (dropped, seen)
        print('PASS OTA reconciles applied BEGIN/WRITE/FINISH/CONFIRM after lost replies', flush=True)


def pipeline_reboot(cli, endpoint, baseline, candidate):
    """Commands following REBOOT cannot prepare or cancel its candidate."""
    import hashlib
    remote = urlparse(endpoint)
    assert remote.scheme == 'tcp'
    data = Path(candidate).read_bytes()

    def reboot_then(op):
        first = bytes((CORE_PROTOCOL_VERSION, 101, 1, 8))
        second = bytes((CORE_PROTOCOL_VERSION, 102, 9, op))
        buffered = bytearray(); accepted = False
        with socket.create_connection((remote.hostname, remote.port), timeout=5) as connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.sendall(encode_frame(1, first) + encode_frame(1, second))
            while True:
                try:
                    chunk = connection.recv(4096)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                buffered.extend(chunk)
                while (frame := UartCommProto._scan(buffered)) is not None:
                    kind, body = frame
                    if kind == 0x41 and body[1:6] == bytes((101, 1, 8, 0, 0)):
                        accepted = True
        assert accepted, 'REBOOT response was not drained'

    # REBOOT during Receiving must not consume a following FINISH and commit it.
    cli('ota-begin', len(data), hashlib.sha256(data).hexdigest())
    for offset in range(0, len(data), 512):
        cli('ota-write', offset, data[offset:offset+512].hex())
    reboot_then(3)
    state = cli('ota-status')
    assert state['elf_sha256'] == Path(baseline).read_bytes()[176:208].hex() and not state['pending_confirm'], state

    # REBOOT during Ready must commit even if ABORT is the next input frame.
    cli('ota-upload', candidate, '--upload-only')
    reboot_then(4)
    state = cli('ota-status')
    assert state['elf_sha256'] == data[176:208].hex() and state['pending_confirm'], state
    restored = cli.reboot()
    assert restored['elf_sha256'] == Path(baseline).read_bytes()[176:208].hex() and not restored['pending_confirm'], restored
    print('PASS pipelined FINISH/ABORT cannot cross the REBOOT commit fence', flush=True)
