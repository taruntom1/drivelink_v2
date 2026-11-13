"""F4c bare-board source smoke and matched 0/16-value link timing.

Main firmware, no attached hardware. Requires a saved loops-only layout and
idle groups. Restores that layout without factory reset or settings changes.
"""
import argparse
import json
from pathlib import Path
import struct
import time

from protocol import CORE_PROTOCOL_VERSION
from smoke_core_firmware import Client
from smoke_safety_firmware import get, wait_frame


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', required=True)
    ap.add_argument('--seconds', type=float, default=35)
    ap.add_argument('--report')
    args = ap.parse_args()
    assert args.seconds >= 5
    c = Client(args.port)
    created = []
    modified = False
    original = c.connect()
    assert not original['flags'] & 11, original
    assert original['count'] == len(original['loops']), 'requires a loops-only layout'
    assert len(original['loops']) >= 2, 'requires two loops'
    assert not wait_frame(c, 0x80, lambda p: p[0] == CORE_PROTOCOL_VERSION)[13] & 8, 'unsaved edits'
    control = next(loop for loop in original['loops'] if loop['realtime'])['id']
    sensor = next(loop for loop in original['loops'] if not loop['realtime'])['id']
    limits = get(c, [(0,0,17), (0,0,21)])
    assert limits[0] >= 16 and limits[1] >= 16, limits
    cursor = 0
    while True:
        page = c.request(5,6,struct.pack('<H',cursor))
        _, cursor, count = struct.unpack_from('<HHB',page)
        pos = 5
        for _ in range(count):
            assert not page[pos+2] & 1, 'group already configured'
            pos += 8 + 4*page[pos+7]
        assert pos == len(page)
        if cursor == 65535:
            break

    def add(type_, label, loop):
        nonlocal modified
        modified = True
        instance = c.add(type_,label,loop)
        created.append((type_,instance))
        return instance

    def set_sources(entries):
        body = bytes((len(entries),)) + b''.join(struct.pack('<BBHI',115,instance,item,value)
                                                for instance,item,value in entries)
        c.request(4,2,body,accepted=(8,))

    def subscribe(loop, refs):
        result = c.request(5,1,struct.pack('<BBHB',0,loop,1,len(refs)) +
                           b''.join(struct.pack('<BBH',*ref) for ref in refs))
        return result[0], result[1]

    def sample(epoch, channel, count):
        p = wait_frame(c, channel, lambda p: len(p) == 12 + 4*count and p[1] == epoch)
        return list(struct.unpack_from('<'+'I'*count,p,12))

    def cleanup():
        nonlocal modified
        # Consumers first, so producer removal respects Referenced.
        for type_, instance in sorted(created, key=lambda entry: entry[0] != 115):
            c.request(2,3,bytes((type_,instance)),accepted=(0,2))
        c.request(1,5)
        info = c.reboot()
        created.clear()
        modified = False
        assert info['count'] == original['count'] and info['loops'] == original['loops'], info

    report = {'seconds_per_run': args.seconds, 'loops': original['loops'], 'runs': []}
    try:
        feedback = add(113,'f4c_feedback',control)
        imu = add(114,'f4c_imu',sensor)
        fusion = add(115,'f4c_fusion',control)
        set_sources([(fusion,1,(113<<24)|(feedback<<16)|256), (fusion,2,(114<<24)|(imu<<16)|256)])
        c.request(1,5); c.reboot()
        epoch, channel = subscribe(control,[(115,fusion,item) for item in range(256,261)])
        values = sample(epoch,channel,5)
        assert values[1] == 0 and values[3] != 0xFFFFFFFF, values
        floats = [struct.unpack('<f',struct.pack('<I',values[i]))[0] for i in (0,2,4)]
        assert abs(floats[2] - (floats[0]+floats[1])) < 1e-5, floats
        cleanup()
        producers = [add(113,f'f4c_p{i}',control) for i in range(8)]
        consumers = [add(115,f'f4c_c{i}',sensor) for i in range(8)]
        # Identical modules, rates, subscription and observation time. Only
        # source refs change between baseline and linked runs.
        for linked in (False, True):
            if linked:
                set_sources([(consumer,item,(113<<24)|(producer<<16)|signal)
                             for producer,consumer in zip(producers,consumers)
                             for item,signal in ((1,256),(2,257))])
            c.request(1,5); c.reboot()
            epoch, channel = subscribe(control,[(5,control,259),(5,control,257),(5,control,256)])
            end = time.monotonic()+args.seconds
            values = None
            while time.monotonic() < end:
                values = sample(epoch,channel,3)
            result = {'linked_values': 16 if linked else 0, 'max_run_us': values[0],
                      'overruns': values[1], 'ticks': values[2]}
            report['runs'].append(result)
            print(json.dumps(result),flush=True)
            assert values[2] > 100 and values[0] > 0, values
            if linked:
                c.request(5,3,b'\x00')
                epoch, channel = subscribe(sensor,[(115,consumers[0],257),(115,consumers[-1],259)])
                assert all(age != 0xFFFFFFFF for age in sample(epoch,channel,2))
        print('PASS source values/ages and matched 16-value link timing',flush=True)
    finally:
        try:
            if modified:
                cleanup()
        finally:
            c.serial.close()
    report['restored'] = True
    if args.report:
        path = Path(args.report); path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(json.dumps(report,indent=2)+'\n')


if __name__ == '__main__':
    main()
