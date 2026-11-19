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
from protocol import CORE_PROTOCOL_VERSION, crc8, crc16_ccitt, encode_frame

# Result / Detail values (protocol/defs/registry.yaml wire_enums).
R_OK, R_INVALID, R_NOT_FOUND, R_BUSY = 0, 1, 2, 5
D_CAPACITY, D_AGENT_VERSION, D_BAD_TOKEN, D_UNKNOWN_DEVICE, D_DEVICE_OFFLINE, D_LEASE_REQUIRED = 30, 54, 55, 56, 57, 58
# Message types (docs/protocol/registry.md).
T_CORE_COMMAND, T_CORE_RESPONSE, T_AGENT_COMMAND, T_AGENT_RESPONSE, T_STATUS, T_SUB0 = 0x01, 0x41, 0x30, 0x70, 0x80, 0x81
AGENT_VERSION = 1
FAM_GROUP, OP_SUB_CONFIG, OP_SUB_CLEAR, OP_LIST = 5, 1, 3, 6

BASE_ID, ARM_ID = '02444c000001', '02444c000002'
PROCS = []   # every child, for cleanup and for failure diagnostics
CLI = None
TOKEN = secrets.token_hex(12)
AGENT_URL = None


class Fail(Exception):
    pass


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
    run = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env)
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

    def __init__(self, name, device='base'):
        self.name, self.seq = name, 0
        host, port = AGENT_URL[len('tcp://'):].rsplit(':', 1)
        self.sock = socket.create_connection((host, int(port)), timeout=5)
        self.buf = bytearray()
        self.frames = collections.defaultdict(lambda: collections.deque(maxlen=1000))
        body = bytes([CORE_PROTOCOL_VERSION]) + b''.join(
            bytes([len(s)]) + s for s in (TOKEN.encode(), device.encode(), name.encode()))
        self.send(T_AGENT_COMMAND, bytes([AGENT_VERSION, self._next(), 1]) + body)
        reply = self.take(T_AGENT_RESPONSE, 5)
        expect(reply[0] == AGENT_VERSION and reply[2] == 1, f'{name}: Hello reply header', reply=reply.hex())
        expect(reply[3] == R_OK, f'{name}: Hello refused', result=reply[3], detail=reply[4])
        self.client_id, kind = struct.unpack_from('<HB', reply, 5)
        expect(kind == 1, f'{name}: expected a device connection', kind=kind)

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

    def pump(self, seconds):
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            ready, _, _ = select.select([self.sock], [], [], max(0.0, left))
            if ready:
                chunk = self.sock.recv(65536)
                if not chunk:
                    fail(f'{self.name}: agent closed the connection')
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
            if time.monotonic() >= deadline:
                fail(f'{self.name}: no frame 0x{msg_type:02x} within {timeout} s')
            self.pump(0.05)

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
    ctx['arm'], url = start_vdev('vdev-arm', ctx['vdev'], ctx['arm_state'], port=ctx['arm_port'], device_id=ARM_ID)
    expect(url == ctx['arm_url'], 'arm restarted on the same endpoint', url=url, wanted=ctx['arm_url'])
    wait_devices(lambda d: d['arm']['online'], 'arm to come back online', timeout=15)
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


def scenario_9_device_clean_after_agent(ctx):
    # The agent clears client groups when they leave. Only now can vdev be
    # asked directly (it serves one client, the agent held it until now).
    code = ctx['agent'].terminate()
    expect(code is not None, 'agent stopped')
    groups = data(direct_retry(ctx['base_url'], 'groups'))['groups']
    expect(not any(g['configured'] for g in groups), 'base has no configured group after the clients left', groups=groups)
    status = data(direct_retry(ctx['base_url'], 'status'))
    expect(status['arm_state'] == 0, 'base is disarmed', status=status)
    return f'after the agent stopped (exit {code}) vdev base has no configured group and is disarmed'


SCENARIOS = [scenario_1_agent_devices, scenario_2_access, scenario_3_same_as_direct, scenario_4_lease_interim,
             scenario_5_groups, scenario_6_status_events, scenario_7_offline_and_back,
             scenario_8_client_sees_device_loss, scenario_9_device_clean_after_agent]


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
                                          '--retry-ms', '300'])
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
