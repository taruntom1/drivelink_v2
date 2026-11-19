#!/usr/bin/env python3
"""drivelink-agent end to end against virtual devices (Linux only, no hardware).

Starts three drivelink-vdev processes and one drivelink-agent that owns two of
them (aliases `base` and `arm`), then drives the shipped CLI and, where the CLI
cannot express a scenario, a minimal raw agent client written here.

  base  vdev with the default device id 02444c000001, owned by the agent
  arm   vdev with device id 02444c000002 (own state dir), owned by the agent
  ref   vdev with the same id/state shape as base, NOT owned by the agent: the
        reference for "through the agent equals directly". A vdev serves one
        TCP client at a time and the agent holds its devices' connection, so
        the agent's devices can never be queried directly while it runs.

Design notes (the reasons are repeated at the scenarios):
  * The CLI is one agent client per process and the agent clears a client's
    group slots when it disconnects. `sub-config` cannot outlive its process
    and `sub-stream` in a later process finds the group unconfigured, so the
    group scenario (5) uses raw clients that keep their connection open.
  * "Directly on vdev" checks for the agent's devices are therefore made
    after the agent has stopped (scenario 9), when vdev accepts a client again.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import queue
import secrets
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'firmware/test/uart_comm'))
from protocol import CORE_PROTOCOL_VERSION, command_group_payload, crc8, crc16_ccitt, encode_frame

# Result / Detail values (protocol/defs/registry.yaml wire_enums).
R_OK, R_INVALID, R_NOT_FOUND, R_BUSY = 0, 1, 2, 5
D_CAPACITY, D_AGENT_VERSION, D_BAD_TOKEN, D_UNKNOWN_DEVICE, D_DEVICE_OFFLINE, D_LEASE_REQUIRED = 30, 54, 55, 56, 57, 58
# Message types (docs/protocol/registry.md).
T_CORE_COMMAND, T_CORE_RESPONSE, T_AGENT_COMMAND, T_AGENT_RESPONSE, T_STATUS, T_SUB0 = 0x01, 0x41, 0x30, 0x70, 0x80, 0x81
AGENT_VERSION = 1
T_DISARM, T_DIAG_EVENT, T_AGENT_EVENT = 0x03, 0xC0, 0xF0
D_UNKNOWN_REF, D_WRONG_STATE = 1, 46
FAM_SYSTEM, OP_ARM, OP_DISARM = 1, 2, 3
FAM_GROUP, OP_SUB_CONFIG, OP_SUB_ENABLE, OP_SUB_CLEAR, OP_CMD_CLEAR, OP_LIST = 5, 1, 2, 3, 5, 6

BASE_ID, ARM_ID = '02444c000001', '02444c000002'
PROCS = []   # every child, for cleanup and for failure diagnostics
CLI = None
TOKEN = secrets.token_hex(12)
AGENT_URL = None


class Fail(Exception):
    pass


def fail(message, **context):
    raise Fail(message + ''.join(f'\n    {k} = {v!r}' for k, v in context.items()))


def expect(condition, message, **context):
    if not condition:
        fail(message + ''.join(f'\n    {k} = {v!r}' for k, v in context.items()))


class Proc:
    """A child process with its stdout/stderr collected by reader threads."""

    def __init__(self, name, argv):
        self.name, self.argv = name, argv
        self.lines, self.errors, self._q = [], [], queue.Queue()
        self.p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self._threads = [threading.Thread(target=self._pump, args=(self.p.stdout, self.lines, True), daemon=True),
                         threading.Thread(target=self._pump, args=(self.p.stderr, self.errors, False), daemon=True)]
        for thread in self._threads:
            thread.start()
        PROCS.append(self)

    def _pump(self, stream, sink, to_queue):
        for line in stream:
            line = line.rstrip('\n')
            sink.append(line)
            if to_queue:
                self._q.put(line)

    def next_line(self, timeout, prefix=''):
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self._q.get(timeout=min(0.2, max(0.01, deadline - time.monotonic())))
                if line.startswith(prefix):
                    return line
            except queue.Empty:
                pass
            if self.p.poll() is not None and self._q.empty():
                fail(f'{self.name} exited ({self.p.returncode}) before printing {prefix!r}', **self.diag())
            if time.monotonic() >= deadline:
                fail(f'{self.name}: no {prefix!r} line within {timeout} s', **self.diag())

    def diag(self):
        return {f'{self.name} stdout': self.lines[-15:], f'{self.name} stderr': self.errors[-15:]}

    def alive(self):
        return self.p.poll() is None

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        for thread in self._threads:
            thread.join(timeout=2)

    def terminate(self, timeout=10):
        if self.p.poll() is None:
            self.p.terminate()
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait(timeout=10)
            fail(f'{self.name} did not stop on SIGTERM within {timeout} s', **self.diag())
        return self.p.returncode


def start_vdev(name, vdev, state, port=0, device_id=None):
    argv = [vdev, '--port', str(port), '--state', str(state)]
    if device_id:
        argv += ['--device-id', device_id]
    proc = Proc(name, argv)
    line = proc.next_line(15, 'READY ')
    return proc, line[len('READY '):]


# ---------------------------------------------------------------- CLI helpers

def cli(endpoint, *command, device=None, token=None, env_token=None, code=0, timeout=20):
    """Run the CLI with --json; return the envelope. Checks the exit code."""
    argv = [CLI, '--endpoint', endpoint]
    if device is not None:
        argv += ['--device', device]
    if token is not None:
        argv += ['--token', token]
    argv += ['--json', *command]
    env = {k: v for k, v in os.environ.items() if k != 'DRIVELINK_AGENT_TOKEN'}
    if env_token is not None:
        env['DRIVELINK_AGENT_TOKEN'] = env_token
    try:
        run = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        fail(f'{command}: the CLI did not finish within {timeout} s')
    lines = run.stdout.strip().splitlines()
    expect(run.returncode == code, f'{command}: exit {run.returncode}, wanted {code}',
           stdout=run.stdout, stderr=run.stderr)
    expect(lines, f'{command}: empty stdout', stderr=run.stderr)
    try:
        envelope = json.loads(lines[-1])
    except ValueError:
        fail(f'{command}: stdout is not a JSON envelope', stdout=run.stdout, stderr=run.stderr)
    expect(envelope['ok'] == (code == 0), f'{command}: ok flag', envelope=envelope)
    return envelope


def direct_retry(endpoint, *command, seconds=8):
    """vdev serves one client and frees it only when it notices the close, so a
    first direct connection after the agent left may be refused: retry reads."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return cli(endpoint, *command)
        except Fail:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)


def via(device, *command, **kw):
    """The CLI through the agent."""
    return cli(AGENT_URL, *command, device=device, token=TOKEN, **kw)


def data(envelope):
    return envelope['data']


def agent_devices(**kw):
    return data(cli(AGENT_URL, 'agent-devices', token=TOKEN, **kw))['devices']


def wait_devices(predicate, description, timeout=30):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = agent_devices()
        if predicate({d['alias']: d for d in last}):
            return {d['alias']: d for d in last}
        time.sleep(0.3)
    fail(f'timeout waiting for {description}', last_agent_devices=last)


# ------------------------------------------------------------ raw agent client

class RawClient:
    """A device connection speaking the agent protocol directly, so a group
    slot stays owned for as long as the test wants (a CLI process cannot)."""

    def __init__(self, name, device='base', rcvbuf=None, hello=True):
        """`device=''` makes a lobby connection; `hello=False` only connects."""
        self.name, self.seq = name, 0
        host, port = AGENT_URL[len('tcp://'):].rsplit(':', 1)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)   # before connect
        self.sock.settimeout(5)
        self.sock.connect((host, int(port)))
        self.buf = bytearray()
        self.frames = collections.defaultdict(lambda: collections.deque(maxlen=1000))
        self.eof, self.rx_bytes = False, 0
        if not hello:
            return
        self.send_hello(device=device)
        reply = self.take(T_AGENT_RESPONSE, 5)
        expect(reply[0] == AGENT_VERSION and reply[2] == 1, f'{name}: Hello reply header', reply=reply.hex())
        expect(reply[3] == R_OK, f'{name}: Hello refused', result=reply[3], detail=reply[4])
        self.client_id, kind = struct.unpack_from('<HB', reply, 5)
        expect(kind == (1 if device else 0), f'{name}: wrong connection kind', kind=kind)

    def send_hello(self, agent_version=AGENT_VERSION, core_version=CORE_PROTOCOL_VERSION, device='base', token=None):
        token = TOKEN if token is None else token
        body = bytes([core_version]) + b''.join(
            bytes([len(s)]) + s for s in (token.encode(), device.encode(), self.name.encode()))
        seq = self._next()
        self.send(T_AGENT_COMMAND, bytes([agent_version, seq, 1]) + body)
        return seq

    def agent_request(self, op, body=b'', timeout=5):
        seq = self._next()
        self.send(T_AGENT_COMMAND, bytes([AGENT_VERSION, seq, op]) + body)
        reply = self.take(T_AGENT_RESPONSE, timeout, lambda f: len(f) >= 5 and f[1] == seq)
        expect(reply[0] == AGENT_VERSION and reply[2] == op, f'{self.name}: agent response header', reply=reply.hex())
        return reply[3], reply[4], reply[5:]

    def wait_eof(self, timeout):
        """Read (keeping frames) until the agent closes; True if it did in time."""
        deadline = time.monotonic() + timeout
        while not self.eof and time.monotonic() < deadline:
            self.pump(min(0.1, max(0.0, deadline - time.monotonic())), eof_ok=True)
        return self.eof

    def _next(self):
        self.seq = self.seq % 250 + 1
        return self.seq

    def send(self, msg_type, payload):
        self.sock.sendall(encode_frame(msg_type, payload))

    def _extract(self):
        while True:
            at = self.buf.find(b'\xaa\xaa\xaa')
            if at < 0:
                del self.buf[:max(0, len(self.buf) - 2)]
                return
            del self.buf[:at]
            if len(self.buf) < 7:
                return
            head = bytes(self.buf[3:6])
            length = struct.unpack_from('<H', head, 1)[0]
            if crc8(head) != self.buf[6] or length > 1024:
                del self.buf[:1]
                continue
            total = 7 + length + 2
            if len(self.buf) < total:
                return
            body = bytes(self.buf[3:7 + length])
            if crc16_ccitt(body) != struct.unpack_from('<H', self.buf, 7 + length)[0]:
                del self.buf[:1]
                continue
            self.frames[head[0]].append(bytes(self.buf[7:7 + length]))
            del self.buf[:total]

    def pump(self, seconds, eof_ok=False):
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if self.eof:
                if not eof_ok:
                    fail(f'{self.name}: agent closed the connection')
                time.sleep(max(0.0, min(left, 0.05)))
                return
            ready, _, _ = select.select([self.sock], [], [], max(0.0, left))
            if ready:
                try:
                    chunk = self.sock.recv(65536)
                except (ConnectionResetError, ConnectionAbortedError):
                    chunk = b''
                if not chunk:
                    self.eof = True
                    if not eof_ok:
                        fail(f'{self.name}: agent closed the connection')
                    return
                self.rx_bytes += len(chunk)
                self.buf += chunk
                self._extract()
            if left <= 0:
                return

    def take(self, msg_type, timeout, match=None):
        deadline = time.monotonic() + timeout
        while True:
            queue_ = self.frames[msg_type]
            for frame in list(queue_):
                if match is None or match(frame):
                    queue_.remove(frame)
                    return frame
            if self.eof:
                fail(f'{self.name}: agent closed the connection before frame 0x{msg_type:02x}')
            if time.monotonic() >= deadline:
                fail(f'{self.name}: no frame 0x{msg_type:02x} within {timeout} s')
            self.pump(0.05, eof_ok=True)

    def request(self, family, op, body=b'', timeout=5):
        seq = self._next()
        self.send(T_CORE_COMMAND, bytes([CORE_PROTOCOL_VERSION, seq, family, op]) + body)
        reply = self.take(T_CORE_RESPONSE, timeout, lambda f: len(f) >= 6 and f[1] == seq)
        return reply[4], reply[5], reply[6:]

    def sub_config(self, group, loop, divider, refs):
        body = struct.pack('<BBHB', group, loop, divider, len(refs)) + b''.join(struct.pack('<BBH', *r) for r in refs)
        return self.request(FAM_GROUP, OP_SUB_CONFIG, body)

    def sub_clear(self, group):
        return self.request(FAM_GROUP, OP_SUB_CLEAR, bytes([group]))

    def list_groups(self):
        entries, start = [], 0
        while True:
            result, detail, body = self.request(FAM_GROUP, OP_LIST, struct.pack('<H', start))
            expect(result == R_OK, f'{self.name}: LIST refused', result=result, detail=detail)
            _total, nxt, count = struct.unpack_from('<HHB', body)
            offset = 5
            for _ in range(count):
                kind, group, flags, loop, epoch, divider, n = struct.unpack_from('<BBBBBHB', body, offset)
                offset += 8
                refs = [struct.unpack_from('<BBH', body, offset + 4 * i) for i in range(n)]
                offset += 4 * n
                entries.append(dict(kind=kind, group=group, flags=flags, loop=loop, epoch=epoch, divider=divider, refs=refs))
            if nxt == 0xFFFF:
                return entries
            start = nxt

    def drain(self, msg_type, seconds):
        self.pump(seconds)
        frames = list(self.frames[msg_type])
        self.frames[msg_type].clear()
        return frames

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------ scenarios

def scenario_1_agent_devices(ctx):
    devices = wait_devices(lambda d: all(a in d and d[a]['online'] for a in ('base', 'arm')),
                           'base and arm to come online')
    expect(list(devices) == ['base', 'arm'], 'agent-devices lists devices in config order', devices=devices)
    for alias, device_id, url in (('base', BASE_ID, ctx['base_url']), ('arm', ARM_ID, ctx['arm_url'])):
        record = devices[alias]
        expect(record['device_id'] == device_id and record['endpoint'] == url and record['clients'] == 0
               and record['name'] == '', f'{alias} record', record=record)
    # Readable form: ALIAS online|offline ID NAME ENDPOINT clients=N
    run = subprocess.run([CLI, '--endpoint', AGENT_URL, '--token', TOKEN, 'agent-devices'],
                         capture_output=True, text=True, timeout=20)
    expect(run.returncode == 0, 'readable agent-devices', stdout=run.stdout, stderr=run.stderr)
    expect(run.stdout.splitlines() == [f'base online {BASE_ID} - {ctx["base_url"]} clients=0',
                                       f'arm online {ARM_ID} - {ctx["arm_url"]} clients=0'],
           'readable agent-devices lines', stdout=run.stdout)
    return 'agent-devices lists base and arm online with their ids (readable and --json)'


def scenario_2_access(ctx):
    bad = cli(AGENT_URL, 'info', device='base', token='0' * 24, code=5)
    expect(bad['detail'] == D_BAD_TOKEN and bad['result'] == R_INVALID, 'wrong token', envelope=bad)
    bad = cli(AGENT_URL, 'agent-devices', token='0' * 24, code=5)
    expect(bad['detail'] == D_BAD_TOKEN, 'wrong token in the lobby', envelope=bad)
    unknown = via('nope', 'info', code=5)
    expect(unknown['detail'] == D_UNKNOWN_DEVICE and unknown['result'] == R_NOT_FOUND, 'unknown device', envelope=unknown)
    for device_id, alias in ((BASE_ID, 'base'), (ARM_ID, 'arm')):
        info = data(via(device_id, 'info'))
        expect(info['device_id'] == device_id, f'info by device id for {alias}', info=info)
        expect(data(via(alias, 'info'))['device_id'] == device_id, f'info by alias {alias}')
    # --device needs a token (option, or DRIVELINK_AGENT_TOKEN when the option is absent).
    cli(AGENT_URL, 'info', device='base', code=2)
    expect(data(cli(AGENT_URL, 'info', device='base', env_token=TOKEN))['device_id'] == BASE_ID, 'token from environment')
    expect(data(cli(AGENT_URL, 'info', device='base', token=TOKEN, env_token='wrong'))['device_id'] == BASE_ID,
           '--token wins over the environment')
    # --device with a serial endpoint, and agent-devices with --device, are usage errors.
    cli('serial:///dev/null', 'info', device='base', token=TOKEN, code=2)
    cli(AGENT_URL, 'agent-devices', device='base', token=TOKEN, code=2)
    # Without --device the token is ignored: a direct vdev endpoint works with any token.
    cli(ctx['ref_url'], 'info', token='ignored')
    return 'wrong token (BadToken), unknown device, device by id/alias, token from environment, usage errors'


def scenario_3_same_as_direct(ctx):
    ref = ctx['ref_url']
    for command in (('info',), ('types',), ('instances',), ('get', '0:0:4')):
        through = data(via('base', *command))
        direct = data(cli(ref, *command))
        expect(through == direct, f'{command[0]} through the agent differs from direct', through=through, direct=direct)
    # A live param: watchdog_ms (0:0:4). Written through the agent, read back
    # through it, compared with the same write made directly on the reference.
    original = data(via('base', 'get', '0:0:4'))['typed_values']
    try:
        through = data(via('base', 'set', '0:0:4=300'))
        direct = data(cli(ref, 'set', '0:0:4=300'))
        expect(through == direct and through['typed_values'] == [300], 'set through the agent', through=through, direct=direct)
        expect(data(via('arm', 'get', '0:0:4'))['typed_values'] == original, 'a write to base must not reach arm')
        expect(data(via('base', 'get', '0:0:4'))['typed_values'] == [300], 'read back through the agent')
    finally:
        via('base', 'set', f'0:0:4={original[0]}')
        cli(ref, 'set', f'0:0:4={original[0]}')
    return 'info, types, instances, get and set through the agent equal a direct vdev'


def scenario_4_lease_interim(ctx):
    refused = via('base', 'arm', code=5)
    expect(refused['detail'] == D_LEASE_REQUIRED and refused['result'] == R_BUSY, 'arm through the agent', envelope=refused)
    refused = via('base', 'cmd-config', '0', '0', code=5)
    expect(refused['detail'] == D_LEASE_REQUIRED, 'cmd-config through the agent', envelope=refused)
    refused = via('base', 'cmd-clear', '0', code=5)
    expect(refused['detail'] == D_LEASE_REQUIRED, 'cmd-clear through the agent', envelope=refused)
    # DISARM is forwarded from every client; two different processes.
    via('base', 'disarm')
    via('base', 'disarm')
    expect(data(via('base', 'status'))['arm_state'] == 0, 'device is not armed')
    return 'arm/cmd-config/cmd-clear refused with LeaseRequired; disarm works from two clients'


def scenario_5_groups(ctx):
    # Raw clients keep their connection, hence their slots. Isolation is shown by:
    #  - each client's LIST view holds only its own group 0, with its own refs
    #    (A: two signals on loop 0, B: one signal on loop 1: different frame
    #    sizes), and its channel reads 0x81 for both although the device slots
    #    differ;
    #  - each stream carries only its own frames: A's are 12+4*2 bytes with its
    #    epoch, B's 12+4*1 bytes with its epoch, none cross over;
    #  - filling the device's whole pool with clients, a further client is refused
    #    Capacity, and clearing one client's group frees exactly one slot;
    #  - the agent clears the groups of disconnected clients: the pool is free
    #    again afterwards, and scenario 9 reads the device itself after the
    #    agent stopped (vdev serves one client, and the agent holds it, so the
    #    device cannot be queried directly while the agent runs).
    slots = data(via('base', 'get', '0:0:18'))['typed_values'][0]
    expect(slots >= 3, 'the device needs at least 3 subscription groups', slots=slots)
    clients = []
    try:
        a, b = RawClient('test-a'), RawClient('test-b')
        clients += [a, b]
        expect(a.client_id != b.client_id, 'client ids are unique', a=a.client_id, b=b.client_id)
        a_refs = [(5, 0, 256), (5, 0, 257)]
        b_refs = [(5, 1, 256)]
        result_a, detail_a, body_a = a.sub_config(0, 0, 10, a_refs)
        result_b, detail_b, body_b = b.sub_config(0, 1, 1, b_refs)
        expect(result_a == R_OK and result_b == R_OK, 'both SUB_CONFIG on group 0 succeed',
               a=(result_a, detail_a), b=(result_b, detail_b))
        expect(body_a[1] == T_SUB0 and body_b[1] == T_SUB0, 'both clients see channel 0x81', a=body_a.hex(), b=body_b.hex())
        epoch_a, epoch_b = body_a[0], body_b[0]
        frames_a, frames_b = a.drain(T_SUB0, 1.0), b.drain(T_SUB0, 1.0)
        expect(len(frames_a) >= 5 and len(frames_b) >= 5, 'both clients receive their frames',
               a=len(frames_a), b=len(frames_b))
        expect(all(len(f) == 12 + 4 * 2 and f[1] == epoch_a for f in frames_a), "A got a frame that isn't its own",
               sizes=sorted({len(f) for f in frames_a}))
        expect(all(len(f) == 12 + 4 * 1 and f[1] == epoch_b for f in frames_b), "B got a frame that isn't its own",
               sizes=sorted({len(f) for f in frames_b}))
        for client, refs, loop, divider in ((a, a_refs, 0, 10), (b, b_refs, 1, 1)):
            view = [e for e in client.list_groups() if e['kind'] == 0]
            expect(len(view) == slots and [e['group'] for e in view] == list(range(slots)),
                   f'{client.name}: LIST shows its own ids 0..{slots - 1}', view=view)
            expect(view[0]['flags'] & 1 and view[0]['refs'] == refs and view[0]['loop'] == loop
                   and view[0]['divider'] == divider, f'{client.name}: group 0 holds its own configuration', group=view[0])
            expect(all(e['flags'] == 0 and e['loop'] == 0xFF and not e['refs'] for e in view[1:]),
                   f'{client.name}: LIST shows no other configured group', view=view)
        # Fill the pool: slots-2 more clients take the remaining device slots.
        for i in range(slots - 2):
            filler = RawClient(f'test-f{i}')
            clients.append(filler)
            result, detail, _ = filler.sub_config(0, 0, 10, [(5, 0, 256)])
            expect(result == R_OK, f'filler {i} takes a free device slot', result=result, detail=detail)
        extra = RawClient('test-extra')
        clients.append(extra)
        result, detail, _ = extra.sub_config(0, 0, 10, [(5, 0, 256)])
        expect((result, detail) == (R_INVALID, D_CAPACITY), 'pool exhausted: Capacity', result=result, detail=detail)
        base = wait_devices(lambda d: d['base']['clients'] == slots + 1, 'client count of base')['base']
        expect(base['clients'] == slots + 1, 'agent-devices counts the connections', base=base)
        # Clearing A's group frees its slot for the refused client; B is unaffected.
        result, _, _ = a.sub_clear(0)
        expect(result == R_OK, 'A clears its group')
        result, detail, _ = extra.sub_config(0, 0, 10, [(5, 0, 256)])
        expect(result == R_OK, 'the freed slot is handed to the waiting client', result=result, detail=detail)
        b.drain(T_SUB0, 0.3)
        expect(len(b.drain(T_SUB0, 0.5)) >= 3, 'B keeps streaming while others change theirs')
    finally:
        for client in clients:
            client.close()
    # All raw clients are gone: the agent clears their slots, so the pool is free again.
    wait_devices(lambda d: d['base']['clients'] == 0, 'clients of base to drop to 0', timeout=5)
    time.sleep(1.0)
    again = []
    try:
        for i in range(slots):
            client = RawClient(f'test-again{i}')
            again.append(client)
            result, detail, _ = client.sub_config(0, 0, 10, [(5, 0, 256)])
            expect(result == R_OK, f'slot {i} is free after the earlier clients left', result=result, detail=detail)
    finally:
        for client in again:
            client.close()
    wait_devices(lambda d: d['base']['clients'] == 0, 'clients of base to drop to 0', timeout=5)
    return f'two clients own their own group 0 ({slots} device slots), streams do not cross, pool exhaustion, slots freed'


def watch_pair(device, command, count):
    procs = [Proc(f'cli-{i}', [CLI, '--endpoint', AGENT_URL, '--device', device, '--token', TOKEN,
                              '--json', command, str(count)]) for i in range(2)]
    for proc in procs:
        try:
            deadline = time.monotonic() + 20
            while proc.alive() and time.monotonic() < deadline:
                time.sleep(0.05)
            expect(not proc.alive(), f'{command} {count} did not finish', **proc.diag())
            expect(proc.p.returncode == 0, f'{command} exit code {proc.p.returncode}', **proc.diag())
        finally:
            proc.kill()
    return [[json.loads(line) for line in proc.lines] for proc in procs]


def scenario_6_status_events(ctx):
    for command in ('status', 'events'):
        for envelopes in watch_pair('base', command, 3):
            expect(len(envelopes) == 3 and all(e['ok'] for e in envelopes), f'{command} 3 envelopes', envelopes=envelopes)
            # `status` keeps only the status stream (msg_type 128). `events` does not
            # filter: on an idle device that stream is what flows, but allow a stray event.
            frames = [e['data']['msg_type'] for e in envelopes]
            expect(all(t == 128 for t in frames) if command == 'status' else 128 in frames,
                   f'{command}: status frames', msg_types=frames)
    return 'two clients each receive status frames through status and events'


def scenario_7_offline_and_back(ctx):
    ctx['arm'].kill()
    devices = wait_devices(lambda d: not d['arm']['online'], 'arm to go offline', timeout=10)
    expect(devices['base']['online'], 'base stays online', devices=devices)
    expect(data(via('base', 'info'))['device_id'] == BASE_ID, 'base still works')
    offline = via('arm', 'info', code=5)
    expect(offline['detail'] == D_DEVICE_OFFLINE and offline['result'] == R_BUSY, 'offline device is refused',
           envelope=offline)
    restart_arm(ctx)
    expect(data(via('arm', 'info'))['device_id'] == ARM_ID, 'info --device arm after the restart')
    return 'killing arm marks it offline (base unaffected); restarting it brings it back'


def scenario_8_client_sees_device_loss(ctx):
    watcher = Proc('cli-watch-arm', [CLI, '--endpoint', AGENT_URL, '--device', 'arm', '--token', TOKEN, '--json', 'status', '1000'])
    try:
        watcher.next_line(15, '{')                    # connected and streaming
        ctx['arm'].kill()
        started = time.monotonic()
        while watcher.alive() and time.monotonic() - started < 10:
            time.sleep(0.05)
        expect(not watcher.alive(), 'the watcher is still running 10 s after the device died', **watcher.diag())
        code = watcher.p.returncode
        expect(code in (3, 4), 'the watcher must fail with a connection (3) or timeout (4) error', exit_code=code,
               **watcher.diag())
    finally:
        watcher.kill()
    return f'a client of arm exits (code {code}) within {time.monotonic() - started:.1f} s of the device dying'


def restart_arm(ctx):
    """Start the arm vdev again on its old endpoint (scenarios 7 and 15)."""
    ctx['arm'], url = start_vdev('vdev-arm', ctx['vdev'], ctx['arm_state'], port=ctx['arm_port'], device_id=ARM_ID)
    expect(url == ctx['arm_url'], 'arm restarted on the same endpoint', url=url, wanted=ctx['arm_url'])
    wait_devices(lambda d: d['arm']['online'], 'arm to come back online', timeout=15)


def sub_pool(ctx):
    return data(via('base', 'get', '0:0:18'))['typed_values'][0]


def core_header(frame):
    """(version, seq, family, op, result, detail) of a six-byte core response."""
    expect(len(frame) == 6, 'core response is six bytes', frame=frame.hex())
    return tuple(frame)


def scenario_10_hello_edge_cases(ctx):
    # (a) a first frame that is not agent_command: closed, nothing said.
    c = RawClient('t10a', hello=False)
    try:
        c.send(T_CORE_COMMAND, bytes([CORE_PROTOCOL_VERSION, 1, 1, 1]))
        expect(c.wait_eof(2), 'a core first frame must close the connection within 2 s')
        expect(c.rx_bytes == 0, 'a core first frame must get no reply', received=c.rx_bytes, buf=bytes(c.buf).hex())
    finally:
        c.close()
    # (b, e) agent version 2 / core version 9 in the Hello: the AgentVersion refusal
    # (the agent's own versions 1 and 10, response version byte 1), then close.
    for what, kwargs in (('agent version 2', dict(agent_version=2)), ('core version 9', dict(core_version=9))):
        c = RawClient('t10b', hello=False)
        try:
            seq = c.send_hello(**kwargs)
            reply = c.take(T_AGENT_RESPONSE, 3)
            wanted = bytes([AGENT_VERSION, seq, 1, R_INVALID, D_AGENT_VERSION, AGENT_VERSION, CORE_PROTOCOL_VERSION])
            expect(reply == wanted, f'{what}: AgentVersion refusal', reply=reply.hex(), wanted=wanted.hex())
            expect(c.wait_eof(2), f'{what}: the agent must close after the refusal')
        finally:
            c.close()
    # (c) a second Hello is WrongState and changes nothing; ListDevices still works.
    for device in ('', 'base'):
        c = RawClient('t10c', device)
        try:
            seq = c.send_hello(device=device)
            reply = c.take(T_AGENT_RESPONSE, 3, lambda f: f[1] == seq)
            expect(list(reply) == [AGENT_VERSION, seq, 1, R_INVALID, D_WRONG_STATE],
                   f'second Hello on a {"device" if device else "lobby"} connection', reply=reply.hex())
            result, detail, body = c.agent_request(2)
            expect(result == R_OK and detail == 0 and body[0] == 2, 'ListDevices after a refused second Hello',
                   result=result, detail=detail, body=body.hex())
            expect(not c.eof, 'the connection stays open')
        finally:
            c.close()
    # (d) no Hello at all, and bytes that never form a frame: closed by the hello timeout
    # (the agent runs with --hello-timeout-ms 500), nothing said.
    for what, junk in (('silence', b''), ('bytes that never form a frame', b'not a frame at all, just some bytes')):
        c = RawClient('t10d', hello=False)
        try:
            started = time.monotonic()
            if junk:
                c.sock.sendall(junk)
            expect(c.wait_eof(3), f'{what}: closed within the hello timeout')
            took = time.monotonic() - started
            expect(took < 2.5 and c.rx_bytes == 0, f'{what}: prompt close without a reply', seconds=took, received=c.rx_bytes)
        finally:
            c.close()
    return 'first frame not a Hello closes silently; AgentVersion refusal (agent v2, core v9) then close; second Hello WrongState; hello timeout'


def scenario_11_disarm_paths(ctx):
    c = RawClient('t11')
    try:
        v = CORE_PROTOCOL_VERSION

        def fast(header_tail, body=b''):
            seq = c._next()
            c.send(T_DISARM, bytes([v, seq]) + header_tail + body)
            return seq, c.take(T_CORE_RESPONSE, 5)

        seq, reply = fast(bytes([FAM_SYSTEM, OP_DISARM]))
        expect(core_header(reply) == (v, seq, FAM_SYSTEM, OP_DISARM, R_OK, 0), 'fast DISARM', reply=reply.hex(), seq=seq)
        seq, reply = fast(bytes([FAM_SYSTEM, OP_ARM]))
        expect(core_header(reply) == (v, seq, 0xFF, 0xFF, R_INVALID, 0), 'fast path with another op (ARM)',
               reply=reply.hex(), seq=seq)
        seq, reply = fast(bytes([FAM_SYSTEM, OP_DISARM]), b'\x00')
        expect(core_header(reply) == (v, seq, FAM_SYSTEM, OP_DISARM, R_INVALID, 0), 'fast DISARM with a body byte',
               reply=reply.hex(), seq=seq)
        result, detail, body = c.request(FAM_SYSTEM, OP_DISARM)
        expect((result, detail, body) == (R_OK, 0, b''), 'system DISARM as a core_command', got=(result, detail, body.hex()))
        # A wrong core version is answered by the agent as a device would.
        seq = c._next()
        c.send(T_CORE_COMMAND, bytes([9, seq, FAM_SYSTEM, 1]))
        reply = c.take(T_CORE_RESPONSE, 5)
        # (A device cannot parse a request of another version, so its reply carries seq 0.)
        expect(core_header(reply) in ((v, seq, 0xFF, 0xFF, R_INVALID, 0), (v, 0, 0xFF, 0xFF, R_INVALID, 0)),
               'core_command with version byte 9', reply=reply.hex(), seq=seq)
        expect(c.request(FAM_SYSTEM, OP_DISARM)[0] == R_OK, 'the connection is still usable')
    finally:
        c.close()
    return 'fast DISARM, wrong-op, body byte, system DISARM and a version-9 core_command answered as a device would'


def scenario_12_interim_rule_raw(ctx):
    c = RawClient('t12')
    try:
        # Command-group streams (0xA0..0xBF) from a client are dropped: no reply, no close.
        c.send(0xA0, command_group_payload(0, 0))
        c.send(0xA0, command_group_payload(1, 7))
        c.send(0xA5, command_group_payload(0, 0))
        c.send(0xBF, command_group_payload(0, 0))
        expect(not c.drain(T_CORE_RESPONSE, 0.5) and not c.drain(T_AGENT_RESPONSE, 0.1), 'command-group frames got a reply')
        result, detail, _ = c.request(FAM_GROUP, OP_CMD_CLEAR, bytes([0]))
        expect((result, detail) == (R_BUSY, D_LEASE_REQUIRED), 'cmd-clear over the raw link', got=(result, detail))
        result, detail, _ = c.request(FAM_SYSTEM, OP_ARM)
        expect((result, detail) == (R_BUSY, D_LEASE_REQUIRED), 'ARM over the raw link', got=(result, detail))
        expect(c.request(FAM_SYSTEM, OP_DISARM)[0] == R_OK, 'the connection is still usable')
    finally:
        c.close()
    return 'command-group frames dropped silently (connection usable); cmd-clear and ARM refused with LeaseRequired'


def scenario_13_unmapped_groups(ctx):
    slots = sub_pool(ctx)
    expect(slots > 3, 'this scenario needs more than 3 subscription groups', slots=slots)
    c = RawClient('t13')
    try:
        result, detail, _ = c.request(FAM_GROUP, OP_SUB_ENABLE, bytes([3, 1]))
        expect((result, detail) == (R_INVALID, D_UNKNOWN_REF), 'SUB_ENABLE on an id without a slot', got=(result, detail))
        result, detail, body = c.sub_clear(3)
        expect((result, detail, body) == (R_OK, 0, bytes([0, 0x84])), 'SUB_CLEAR on an id without a slot',
               got=(result, detail, body.hex()))
        for group in (slots, 255):
            result, detail, _ = c.sub_config(group, 0, 10, [(5, 0, 256)])
            expect((result, detail) == (R_INVALID, D_CAPACITY), f'SUB_CONFIG with id {group} >= pool size {slots}',
                   got=(result, detail))
        view = [e for e in c.list_groups() if e['kind'] == 0]
        expect(not any(e['flags'] for e in view), 'nothing got configured', view=view)
    finally:
        c.close()
    return f'unmapped ids: ENABLE UnknownRef, CLEAR Ok [epoch 0, 0x84], CONFIG beyond the pool of {slots} Capacity'


def scenario_14_device_reboot(ctx):
    # vdev's reboot closes its TCP link after the reply (host/vdev/README.md; only its PTY
    # survives a reboot), so the agent sees the link fail rather than a Boot event, which
    # vdev emits into the rebuilt core with no client attached. What can be shown: the
    # device connection gets Closing (DeviceOffline) and then EOF, its group stream ends,
    # the device comes back, and a fresh client's group works with LIST showing only it.
    # A diag Boot (0xC0, kind 4) is accepted if one does arrive.
    refs = [(5, 0, 256), (5, 0, 257)]
    a = RawClient('t14a')
    try:
        result, detail, _ = a.sub_config(0, 0, 10, refs)
        expect(result == R_OK, 'group 0 before the reboot', result=result, detail=detail)
        expect(len(a.drain(T_SUB0, 0.6)) >= 3, 'frames flow before the reboot')
        via('base', 'reboot')
        expect(a.wait_eof(10), 'the connection must close when the device reboots')
        closing = [bytes(f) for f in a.frames[T_AGENT_EVENT]]
        expect(bytes([1, 1, 1]) in closing, 'Closing(DeviceOffline) before EOF', events=[f.hex() for f in closing])
        boot = [bytes(f) for f in a.frames[T_DIAG_EVENT] if len(f) > 3 and f[3] == 4]
    finally:
        a.close()
    wait_devices(lambda d: d['base']['online'], 'base to come back after the reboot', timeout=20)
    deadline = time.monotonic() + 15
    while True:
        try:
            expect(data(via('base', 'info'))['device_id'] == BASE_ID, 'info after the reboot')
            break
        except Fail:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.3)
    b = RawClient('t14b')
    try:
        result, detail, _ = b.sub_config(0, 0, 10, refs)
        expect(result == R_OK, 'a fresh SUB_CONFIG after the reboot', result=result, detail=detail)
        expect(len(b.drain(T_SUB0, 1.0)) >= 5, 'frames flow again after the reboot')
        view = [e for e in b.list_groups() if e['kind'] == 0]
        expect(view[0]['flags'] & 1 and view[0]['refs'] == refs and not any(e['flags'] for e in view[1:]),
               'LIST shows only the new group', view=view)
    finally:
        b.close()
    wait_devices(lambda d: d['base']['clients'] == 0, 'clients of base to drop to 0', timeout=5)
    return ('reboot: Closing(DeviceOffline) + EOF, stream stopped, device back, new group streams, LIST shows only it'
            + (' (a diag Boot arrived too)' if boot else ' (vdev drops TCP, so no diag Boot reaches the client)'))


def scenario_15_closing_events(ctx):
    if not ctx['arm'].alive():     # scenario 8 left it dead
        restart_arm(ctx)
    r = RawClient('t15', 'arm')
    try:
        ctx['arm'].kill()
        expect(r.wait_eof(10), 'the connection to a dead device must close')
        events = [bytes(f) for f in r.frames[T_AGENT_EVENT]]
        expect(events == [bytes([1, 1, 1])], 'Closing(DeviceOffline) before EOF', events=[f.hex() for f in events])
    finally:
        r.close()
    restart_arm(ctx)
    return 'a raw client of arm gets agent_event [1, 1, 1] (Closing, DeviceOffline) before EOF when the device dies'


def scenario_16_slow_client(ctx):
    # A client that subscribes to a fast group and never reads while flooding LIST requests
    # (small SO_RCVBUF so the agent's side fills quickly). The agent must keep serving
    # everyone else; it may close the slow client or keep it, either is accepted.
    started = time.monotonic()
    refs = [(5, 0, 256), (5, 0, 257)] * 4
    slow = RawClient('t16-slow', rcvbuf=4096)
    try:
        result, detail, _ = slow.sub_config(0, 0, 1, refs)
        if result != R_OK:
            refs = refs[:2]
            result, detail, _ = slow.sub_config(0, 0, 1, refs)
        expect(result == R_OK, 'the slow client subscribes', result=result, detail=detail)
        slow.sock.setblocking(False)
        pending, queued, closed_during = b'', 0, False
        last_check, checks = time.monotonic(), 0
        while time.monotonic() - started < 8 and not closed_during:
            if len(pending) < 4096:
                for _ in range(100):
                    seq = slow._next()
                    pending += encode_frame(T_CORE_COMMAND, bytes([CORE_PROTOCOL_VERSION, seq, FAM_GROUP, OP_LIST, 0, 0]))
                    queued += 1
            _, writable, _ = select.select([], [slow.sock], [], 0.02)
            if writable:
                try:
                    pending = pending[slow.sock.send(pending):]
                except BlockingIOError:
                    pass
                except OSError:
                    closed_during = True
            if time.monotonic() - last_check >= 2:
                expect(data(via('base', 'info', timeout=10))['device_id'] == BASE_ID, 'info while a client is stuck')
                expect(len(agent_devices()) == 2, 'agent-devices while a client is stuck')
                checks += 1
                last_check = time.monotonic()
        expect(queued >= 2000 or closed_during, 'enough LIST requests were sent', queued=queued)
        expect(checks >= 2 or closed_during, 'the agent was probed while flooded', checks=checks)
        # Read what the agent kept for it, until it closes or 3 s pass.
        read_until = time.monotonic() + 3
        while time.monotonic() < read_until and not slow.eof:
            ready, _, _ = select.select([slow.sock], [], [], 0.1)
            if ready:
                try:
                    chunk = slow.sock.recv(65536)
                except BlockingIOError:
                    continue
                except OSError:
                    chunk = b''
                if not chunk:
                    slow.eof = True
        outcome = 'closed by the agent' if (slow.eof or closed_during) else 'kept'
        expect(data(via('base', 'info', timeout=10))['device_id'] == BASE_ID, 'info after the flood')
        expect(len(agent_devices()) == 2, 'agent-devices after the flood')
    finally:
        slow.close()
    wait_devices(lambda d: d['base']['clients'] == 0, 'clients of base to drop to 0', timeout=10)
    time.sleep(0.5)
    again = RawClient('t16-again')
    try:
        result, detail, _ = again.sub_config(0, 0, 10, [(5, 0, 256)])
        expect(result == R_OK, 'the stuck client leaked no slot', result=result, detail=detail)
    finally:
        again.close()
    return (f'{queued} LISTs flooded by a client that never reads; agent stayed responsive ({checks} checks); '
            f'slow client {outcome}')


def scenario_9_device_clean_after_agent(ctx):
    # The agent clears client groups when they leave. Only now can vdev be
    # asked directly (it serves one client, the agent held it until now).
    lobby = RawClient('t9-lobby', '')
    code = ctx['agent'].terminate()
    expect(code is not None, 'agent stopped')
    try:
        expect(lobby.wait_eof(5), 'the lobby connection closes when the agent stops')
        events = [bytes(f) for f in lobby.frames[T_AGENT_EVENT]]
        expect(events == [bytes([1, 1, 2])], 'Closing(Shutdown) before EOF', events=[f.hex() for f in events])
    finally:
        lobby.close()
    groups = data(direct_retry(ctx['base_url'], 'groups'))['groups']
    expect(not any(g['configured'] for g in groups), 'base has no configured group after the clients left', groups=groups)
    status = data(direct_retry(ctx['base_url'], 'status'))
    expect(status['arm_state'] == 0, 'base is disarmed', status=status)
    return f'after the agent stopped (exit {code}) vdev base has no configured group and is disarmed'


SCENARIOS = [scenario_1_agent_devices, scenario_2_access, scenario_3_same_as_direct, scenario_4_lease_interim,
             scenario_5_groups, scenario_6_status_events, scenario_7_offline_and_back,
             scenario_8_client_sees_device_loss, scenario_10_hello_edge_cases, scenario_11_disarm_paths,
             scenario_12_interim_rule_raw, scenario_13_unmapped_groups, scenario_14_device_reboot,
             scenario_15_closing_events, scenario_16_slow_client, scenario_9_device_clean_after_agent]


def main():
    global CLI, AGENT_URL
    ap = argparse.ArgumentParser()
    ap.add_argument('--agent', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--vdev', required=True)
    args = ap.parse_args()
    if not sys.platform.startswith('linux'):
        print('SKIP agent integration is Linux only')
        return 0
    CLI = args.cli
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='drivelink-agent-it-') as tmp:
        tmp = Path(tmp)
        ctx = {'vdev': args.vdev, 'arm_state': tmp / 'arm'}
        try:
            ctx['base'], ctx['base_url'] = start_vdev('vdev-base', args.vdev, tmp / 'base')
            ctx['arm'], ctx['arm_url'] = start_vdev('vdev-arm', args.vdev, ctx['arm_state'], device_id=ARM_ID)
            ctx['ref'], ctx['ref_url'] = start_vdev('vdev-ref', args.vdev, tmp / 'ref')
            ctx['arm_port'] = int(ctx['arm_url'].rsplit(':', 1)[1])
            ctx['agent'] = Proc('agent', [args.agent, '--port', '0', '--token', TOKEN,
                                          '--device', f'base={ctx["base_url"]}', '--device', f'arm={ctx["arm_url"]}',
                                          '--retry-ms', '300', '--hello-timeout-ms', '500'])
            AGENT_URL = ctx['agent'].next_line(15, 'READY ')[len('READY '):]
            expect(AGENT_URL.startswith('tcp://127.0.0.1:'), 'agent READY line', line=AGENT_URL)
            for scenario in SCENARIOS:
                summary = scenario(ctx)
                print(f'PASS {scenario.__name__}: {summary}', flush=True)
            print(f'PASS agent integration ({len(SCENARIOS)} scenarios, {time.monotonic() - started:.0f} s)')
            return 0
        except Fail as failure:
            print(f'FAIL {failure}', file=sys.stderr)
            for proc in PROCS:
                if proc.name in ('agent',) or proc.name.startswith('vdev'):
                    for key, lines in proc.diag().items():
                        print(f'  {key}: {lines}', file=sys.stderr)
            return 1
        finally:
            for proc in PROCS:
                proc.kill()


if __name__ == '__main__':
    sys.exit(main())
