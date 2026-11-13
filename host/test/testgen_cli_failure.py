"""Regression: an applied ARM with a lost reply must still cause cleanup DISARM.

Proxy the real vdev so all descriptors, limits, groups and ARM validation use
the production protocol. Only the successful ARM reply is intentionally lost.
"""
import json
from pathlib import Path
import socket
import subprocess
import sys
import threading
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware/test/uart_comm'))
from protocol import UartCommProto, encode_frame


def run(cli_path, endpoint, target):
    remote = urlparse(endpoint)
    assert remote.scheme == 'tcp'
    stopped = threading.Event()
    seen = {'applied_arm': False, 'cleanup_disarm': False}
    errors = []
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(10)

    def pump(source, destination, incoming):
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
                    if incoming and kind == 0x41 and len(body) >= 6 and body[2:4] == bytes((1, 2)):
                        seen['applied_arm'] = body[4:6] == bytes((0, 0))
                        continue
                    if not incoming and kind == 0x03 and len(body) == 4 and body[2:4] == bytes((1, 3)):
                        seen['cleanup_disarm'] = True
                    destination.sendall(encode_frame(kind, body))
        except (ConnectionResetError, BrokenPipeError):
            pass  # normal Session close can reset a TCP connection on Windows
        except OSError as error:
            if not stopped.is_set():
                errors.append(str(error))
        finally:
            stopped.set()

    def proxy():
        try:
            with listener.accept()[0] as client, socket.create_connection((remote.hostname, remote.port), timeout=5) as device:
                client.settimeout(.1); device.settimeout(.1)
                forward = threading.Thread(target=pump, args=(client, device, False))
                reverse = threading.Thread(target=pump, args=(device, client, True))
                forward.start(); reverse.start(); forward.join(10); reverse.join(10)
                assert not forward.is_alive() and not reverse.is_alive(), 'proxy did not stop'
        except Exception as error:
            errors.append(str(error)); stopped.set()

    worker = threading.Thread(target=proxy)
    worker.start()
    try:
        completed = subprocess.run([str(cli_path), '--endpoint', f'tcp://127.0.0.1:{listener.getsockname()[1]}',
            '--timeout-ms', '250', '--json', 'testgen-run', '0', '20', '5000',
            '0', target, 'step', '0', '.1', '0', '0', '1000', '1'],
            capture_output=True, text=True, timeout=15)
    finally:
        stopped.set(); listener.close(); worker.join(12)
    assert not worker.is_alive() and not errors, errors
    result = json.loads(completed.stdout)
    assert completed.returncode == 4 and not result['ok'], (completed.returncode, result)
    assert seen['applied_arm'] and seen['cleanup_disarm'], seen
    print('PASS testgen-run sends DISARM after an applied ARM reply is lost', flush=True)
