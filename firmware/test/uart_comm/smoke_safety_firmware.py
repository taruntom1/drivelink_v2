"""F4b bare-board smoke; creates one mock and restores the saved layout/settings.

Normal: --port COM3. Bench build: add --bench; --wdt also tests panic-reset.
Nothing may be attached to the board. Do not run alongside another port owner.
"""
import argparse
from collections import deque
import struct
import time

from protocol import CORE_PROTOCOL_VERSION, command_group_payload
from smoke_core_firmware import Client


def get(c, refs):
    body = bytes((len(refs),)) + b''.join(struct.pack('<BBH', *r) for r in refs)
    p = c.request(4, 1, body)
    assert p[0] == len(refs) and len(p) == 1 + 4 * len(refs)
    return list(struct.unpack_from('<' + 'I' * len(refs), p, 1))


def set_one(c, ref, value):
    c.request(4, 2, b'\x01' + struct.pack('<BBHI', *ref, value))


def wait_frame(c, kind, predicate, timeout=2, keepalive=None):
    end = time.monotonic() + timeout
    count, last = 0, deque(maxlen=3)
    while time.monotonic() < end:
        if keepalive:
            keepalive()
        try:
            msg, p = c.proto.read_frame(timeout=min(.03, max(.001, end-time.monotonic())))
        except TimeoutError:
            continue
        count += 1
        last.append((msg, p.hex()))
        if msg == kind and predicate(p):
            return p
    raise TimeoutError(f'expected frame {kind:#x} did not arrive; '
                       f'count={count}, last_frames={list(last)}')


def state(c, expected, reason=None, **kwargs):
    return wait_frame(c, 0x80, lambda p: p[0] == CORE_PROTOCOL_VERSION and p[12] == expected
                      and (reason is None or p[-1] == reason), **kwargs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', required=True)
    ap.add_argument('--bench', action='store_true')
    ap.add_argument('--wdt', action='store_true')
    ap.add_argument('--any-core', action='store_true', help='run the real-time control loop with automatic initial placement')
    ap.add_argument('--stack-wait', type=float, default=0, help='disarmed seconds before cleanup for console stack marks')
    args = ap.parse_args()
    assert not args.wdt or args.bench, '--wdt requires --bench'
    c = Client(args.port)
    motor = None
    settings = None
    original = c.connect()
    assert not original['flags'] & 11, 'pending edits, invalid layout or safe mode'
    # Refuse to discard another client's ephemeral group configuration.
    cursor = 0
    while True:
        page = c.request(5, 6, struct.pack('<H', cursor))
        _, cursor, count = struct.unpack_from('<HHB', page)
        pos = 5
        for _ in range(count):
            assert not page[pos+2] & 1, 'a group is in use; smoke did not modify it'
            pos += 8 + 4 * page[pos+7]
        assert pos == len(page)
        if cursor == 65535:
            break
    control = next(l for l in original['loops'] if l['realtime'])
    refs = [(0, 0, 3), (0, 0, 4)]
    core_ref = (5, control['id'], 3)
    original_core = get(c, [core_ref])[0]
    try:
        settings = get(c, refs)
        limits = get(c, [(0,0,i) for i in range(16,21)])
        assert all(limits), limits
        c.request(4,2,b'\x01'+struct.pack('<BBHI',0,0,16,1),accepted=(1,))
        motor = c.add(112, 'safety_mock', control['id'])
        if args.any_core:
            c.request(4, 2, b'\x01' + struct.pack('<BBHI', *core_ref, 2), accepted=(8,))
        c.request(1,5); c.reboot()
        if args.any_core:
            assert get(c, [core_ref]) == [2]
        set_one(c, refs[0], 100)
        set_one(c, refs[1], 200)
        group = c.request(5,4,bytes((0,control['id'],1))+struct.pack('<BBH',112,motor,512))
        epoch, channel = group
        sub = c.request(5,1,struct.pack('<BBHB',0,control['id'],max(1,control['rate_hz']//100),2)
                        + struct.pack('<BBHBBH',112,motor,512,112,motor,256))
        sub_epoch, sub_channel = sub
        counter = 0

        def send(value=None):
            nonlocal counter
            values = () if value is None else (struct.unpack('<I',struct.pack('<f',value))[0],)
            c.proto.send_command(channel, command_group_payload(epoch,counter,values))
            counter = (counter+1)&65535

        def sample(value, **kwargs):
            return wait_frame(c, sub_channel, lambda p: len(p)==20 and p[1]==sub_epoch
                              and abs(struct.unpack_from('<f',p,12)[0]-value)<1e-5, **kwargs)

        c.request(1,2); send(.6)
        state(c,1,keepalive=send)
        p = sample(.6,keepalive=send)
        assert struct.unpack_from('<f',p,16)[0]>0
        c.request(1,5,accepted=(6,))
        c.request(5,5,b'\x00',accepted=(6,))
        c.request(4,2,b'\x01'+struct.pack('<BBHI',0,0,4,50),accepted=(6,))
        c.request(1,3,msg_type=0x03); state(c,0); sample(0)
        send(.9)  # old values must not reappear when only keepalives follow ARM
        c.request(1,2); state(c,1,keepalive=send); sample(0,keepalive=send)
        c.request(1,3,msg_type=0x03)
        print('PASS arming, command application, stale mailbox gating, armed refusals and fast DISARM',flush=True)

        c.action(112,motor,770,(0,1,0,0)); c.request(1,2,accepted=(9,))
        c.action(112,motor,770,(0,0,0,0)); c.request(1,2)
        c.action(112,motor,770,(1,1,0,0)); state(c,2)
        c.request(1,4,accepted=(9,))
        c.action(112,motor,770,(1,0,0,0)); c.request(1,2,accepted=(9,))
        c.request(1,4); c.request(1,2); c.request(1,3,msg_type=0x03)
        print('PASS arm blockers, synchronous faults, active refusal, recovery and ACK_FAULT',flush=True)

        set_one(c,refs[1],50)
        c.request(1,2)
        p = state(c,0,reason=2)
        assert p[-1]==2,p.hex()
        c.request(1,2); c.request(1,3,msg_type=0x03)
        set_one(c,refs[1],200)
        print('PASS watchdog disarm and re-arm without fault acknowledgement',flush=True)

        if args.bench:
            # Test both a real-time and a background heartbeat while RX and
            # esp_timer keep running independently of the deliberately busy loop.
            targets = [control] + [l for l in original['loops'] if not l['realtime']][:1]
            for target in targets:
                c.request(1,2); send(.5)
                c.request(1,253,struct.pack('<BH',target['id'],150))
                p = state(c,2,reason=4)
                assert p[-1]==4,p.hex()
                time.sleep(.25)
                c.request(1,4)
                print(f"PASS loop {target['id']} stall, independent monitor and heartbeat recovery",flush=True)
            if args.wdt:
                c.request(1,2)
                c.request(1,253,struct.pack('<BH',control['id'],10000))
                p = state(c,2,reason=4); assert p[-1]==4,p.hex()
                time.sleep(7)
                info = c.connect()
                assert info['count']==original['count']+1 and not info['flags']&8,info
                state(c,0)
                print('PASS Task WDT reboot; verify Task watchdog panic on the console log',flush=True)
        else:
            c.request(1,253,struct.pack('<BH',control['id'],10),accepted=(1,))
        if args.stack_wait:
            time.sleep(args.stack_wait)
    finally:
        if motor is not None:
            c.request(1,3,msg_type=0x03)
            for ref, value in zip(refs, settings):
                set_one(c,ref,value)
            c.request(2,3,bytes((112,motor)))
            if args.any_core:
                c.request(4, 2, b'\x01' + struct.pack('<BBHI', *core_ref, original_core), accepted=(8,))
            c.request(1,5)
            restored=c.reboot()
            assert restored['count']==original['count'],restored
            assert get(c, [core_ref]) == [original_core]
            print('PASS saved layout/settings restored; temporary groups removed by reboot',flush=True)
        c.serial.close()


if __name__ == '__main__':
    main()
