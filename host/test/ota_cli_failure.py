"""OTA workflow regressions through a real vdev frame proxy (TCP only)."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware/test/uart_comm'))
from protocol import UartCommProto, encode_frame, CORE_PROTOCOL_VERSION


def check(cli_path, endpoint, image, mismatch=False, hold_commit=False, boot_reconnect=False):
    remote = urlparse(endpoint)
    if remote.scheme != 'tcp':
        return  # TCP case runs on both OSes; PTY still runs the shared campaign.
    stopped = threading.Event()
    listener = socket.socket(); listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(.1)
    endpoint = f'tcp://127.0.0.1:{listener.getsockname()[1]}'
    seen = {'confirm': False, 'mismatch': False, 'reboot': 0, 'boot_injected': False}
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
                    if incoming and boot_reconnect and not seen['boot_injected'] and seen['reboot'] and kind == 0x41 and body[2:4] == bytes((1, 1)):
                        # UART can deliver Boot while reconnect GET_INFO is in
                        # flight. Restart only readiness reads, never REBOOT.
                        boot = struct.pack('<BHBBQBBBB', 3, 0, 4, 0, 0, 0, 2, 0, 0)
                        with client_write:
                            destination.sendall(encode_frame(0xc0, boot) + encode_frame(kind, body))
                        seen['boot_injected'] = True
                        continue
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
        if boot_reconnect:
            assert seen['boot_injected'] and seen['reboot'] == 1, seen
        print('PASS OTA reconciles applied BEGIN/WRITE/FINISH/CONFIRM after lost replies', flush=True)
        if boot_reconnect:
            print('PASS OTA restarts Boot-interrupted readiness reads without repeating REBOOT', flush=True)


def pipeline_reboot(cli, endpoint, baseline, candidate):
    """Commands following REBOOT cannot prepare or cancel its candidate."""
    import hashlib
    remote = urlparse(endpoint)
    assert remote.scheme == 'tcp'
    data = Path(candidate).read_bytes()

    def admitted_connection():
        # A previous CLI may have exited before vdev processes its EOF. Only
        # retry this read-only handshake, on the socket that will own REBOOT.
        deadline = time.monotonic() + 5
        attempts, last_error = 0, None
        while time.monotonic() < deadline:
            attempts += 1
            connection = None
            try:
                connection = socket.create_connection((remote.hostname, remote.port),
                                                      timeout=max(.001, min(1, deadline - time.monotonic())))
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                connection.sendall(encode_frame(1, bytes((CORE_PROTOCOL_VERSION, 100, 1, 1))))
                buffered = bytearray()
                while True:
                    connection.settimeout(max(.001, min(1, deadline - time.monotonic())))
                    chunk = connection.recv(4096)
                    if not chunk:
                        raise ConnectionError('EOF before GET_INFO admission')
                    buffered.extend(chunk)
                    while (frame := UartCommProto._scan(buffered)) is not None:
                        kind, body = frame
                        if kind == 0x41 and body[:6] == bytes((CORE_PROTOCOL_VERSION, 100, 1, 1, 0, 0)):
                            return connection, buffered, attempts
                    if time.monotonic() >= deadline:
                        raise TimeoutError('GET_INFO admission deadline')
            except OSError as error:
                last_error = error
                if connection is not None:
                    connection.close()
                time.sleep(.02)
        raise AssertionError(f'GET_INFO admission failed after {attempts} attempts: {last_error}')

    def reboot_then(op):
        first = bytes((CORE_PROTOCOL_VERSION, 101, 1, 8))
        second = bytes((CORE_PROTOCOL_VERSION, 102, 9, op))
        connection, buffered, attempts = admitted_connection()
        accepted, closed, headers, terminal = False, False, [], 'EOF'
        deadline = time.monotonic() + 5
        with connection:
            # Never retry once either mutation may have been submitted.
            connection.sendall(encode_frame(1, first) + encode_frame(1, second))
            while True:
                try:
                    connection.settimeout(max(.001, deadline - time.monotonic()))
                    chunk = connection.recv(4096)
                except ConnectionResetError as error:
                    # vdev must close with FIN after the reply. An RST can
                    # discard the unread reply on Windows, so it is a failure.
                    terminal = str(error)
                    break
                except TimeoutError as error:
                    terminal = str(error)
                    break
                if not chunk:
                    closed = True
                    break
                buffered.extend(chunk)
                while (frame := UartCommProto._scan(buffered)) is not None:
                    kind, body = frame
                    if len(headers) < 16:
                        headers.append((kind, body[:6].hex()))
                    if kind == 0x41 and body[:6] == bytes((CORE_PROTOCOL_VERSION, 101, 1, 8, 0, 0)):
                        accepted = True
                if time.monotonic() >= deadline:
                    terminal = 'REBOOT drain deadline'
                    break
        assert accepted and closed, ('REBOOT response/close missing after admission', attempts, headers, terminal)

    # REBOOT during Receiving must not consume a following FINISH and commit it.
    cli('ota-begin', len(data), hashlib.sha256(data).hexdigest())
    for offset in range(0, len(data), 512):
        cli('ota-write', offset, data[offset:offset+512].hex())
    reboot_then(3)
    state = cli('ota-status')
    assert state['state'] == 0 and state['elf_sha256'] == Path(baseline).read_bytes()[176:208].hex() and not state['pending_confirm'], state

    # REBOOT during Ready must commit even if ABORT is the next input frame.
    cli('ota-upload', candidate, '--upload-only')
    reboot_then(4)
    state = cli('ota-status')
    assert state['elf_sha256'] == data[176:208].hex() and state['pending_confirm'], state
    restored = cli.reboot()
    assert restored['elf_sha256'] == Path(baseline).read_bytes()[176:208].hex() and not restored['pending_confirm'], restored
    print('PASS pipelined FINISH/ABORT cannot cross the REBOOT commit fence', flush=True)
