"""Run the real cascade controller against mocks over TCP or bare-board UART.

Real-motor first run (F5a-2 supplies its module setup): wheels off the ground,
current-limited supply, low duty cap, hand on the power switch. Never infer
physical stopping from a zero command alone. F5b adds the loop-stall scenario.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def run(cli_path, endpoint, loop_rate=50, report=None, stack_wait=0):
    def cli(*command, code=0):
        for attempt in range(10):
            p = subprocess.run([str(cli_path), '--endpoint', endpoint, '--json', *map(str, command)],
                               capture_output=True, text=True, timeout=20)
            result = json.loads(p.stdout)
            # Withdrawn guarantees no mutation; an unknown outcome is never retried.
            if code == 0 and result.get('result') == 5 and result.get('detail') == 15 and attempt < 9:
                time.sleep(.02)
                continue
            break
        assert p.returncode == code, (command, p.returncode, p.stdout, p.stderr)
        assert result['ok'] == (code == 0), result
        return result.get('data', result)

    def connect():
        deadline = time.monotonic() + 15
        while True:
            try:
                return cli('info')
            except (AssertionError, json.JSONDecodeError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.3)

    info = connect()
    assert not info['flags'] & 11, 'pending edits, invalid layout or safe mode'
    assert not cli('status')['flags'] & 8, 'unsaved params'
    original = cli('instances')
    assert all(x['type'] == 5 for x in original), 'requires an original loops-only layout'
    assert all(not g['configured'] for g in cli('groups')['groups']), 'requires idle groups'
    refs = [f"5:{loop['id']}:1" for loop in info['loops']] + ['0:0:4']
    settings = cli('get', *refs)['typed_values']
    created = []
    trace = []
    try:
        for type_, label in ((112, 'motor_smoke'), (113, 'encoder_smoke'), (32, 'pid_smoke')):
            created.append((type_, cli('add', type_, label, 0)['instance']))
        motor, encoder, controller = [i for _, i in created]
        pid = f'32:{controller}'
        cli('set', f'113:{encoder}:1={(112 << 8) | motor}',
            f'{pid}:1={(112 << 8) | motor}', f'{pid}:2={(113 << 8) | encoder}')
        cli('save'); cli('reboot'); connect()
        assert cli('get', f'{pid}:1', f'{pid}:2')['typed_values'] == [(112 << 8) | motor, (113 << 8) | encoder]
        rates = [f"5:{loop['id']}:1={loop_rate if loop['id'] == 0 else (25 if endpoint.startswith('tcp://') else loop['rate_hz'])}" for loop in info['loops']]
        cli('set', *rates, '0:0:4=1000')
        cli('set', f'{pid}:32=0.12', f'{pid}:33=0.8', f'{pid}:48=3', f'{pid}:49=2',
            f'{pid}:17=0.4', f'{pid}:18=4')
        cli('save'); cli('reboot'); connect()
        assert cli('get', f'{pid}:32', f'{pid}:49')['typed_values'][1] == 2
        descriptors = {x['name']: x for x in cli('items', 32)}
        assert descriptors['waiting_for_command']['value_type'] == 0
        cli('cmd-config', 0, 0, f'{pid}:514', f'{pid}:512', f'{pid}:513')
        cli('sub-config', 0, 0, max(1, loop_rate // 50),
            *(f'{pid}:{i}' for i in range(256, 261)), f'112:{motor}:258')
        counter = 0

        def command(values=None):
            nonlocal counter
            cli('cmd-send', 0, counter & 65535, *(values or [])); counter += 1

        def sample():
            s = cli('sub-stream', 0)['typed_values']
            trace.append(s)
            assert abs(s[2]) <= .40001 and abs(s[3]) <= 4.00001, s
            return s

        def drive(values, duration):
            end = time.monotonic() + duration
            samples = []
            while time.monotonic() < end:
                command(values); samples.append(sample()); time.sleep(.02)
            return samples

        cli('arm')
        drive([.2, 2, 0], .5)
        cli('set', f'{pid}:16=1')
        # Old velocity target was present in every open-loop frame. Keepalives
        # cannot turn it into a new-mode command.
        for _ in range(3):
            command(); assert sample()[4] is True
        velocity = drive([0, 2, 0], 6)
        assert abs(sum(s[1] for s in velocity[-5:])/len(velocity[-5:]) - 2) < .3, velocity[-5:]
        assert velocity[-1][4] is False
        cli('set', f'{pid}:16=2')
        command(); assert sample()[4] is True
        target = sample()[0] + .5
        position = drive([0, 0, target], 8)
        assert abs(position[-1][0] - target) < .12, (target, position[-5:])
        assert abs(position[-1][1]) < .3, position[-5:]
        # No receipt, including a keepalive, follows: normal watchdog must stop.
        time.sleep(1.2)
        deadline = time.monotonic() + 2
        observed = []
        while True:
            status = cli('status'); observed.append(status)
            if status['arm_state'] != 1 or time.monotonic() >= deadline:
                break
            time.sleep(.05)
        assert status['arm_state'] == 0 and status['last_disarm_reason'] == 2, observed
        stopped = sample(); assert stopped[2] == 0 and stopped[5] == 0, stopped
        cli('arm'); command(); assert sample()[2] == 0
        cli('disarm')
        if stack_wait:
            time.sleep(stack_wait)
        print('PASS cascade velocity/position, mode freshness, persistence, watchdog and re-arm')
    finally:
        cli('disarm')
        cli('cmd-clear', 0); cli('sub-clear', 0)
        for type_, instance in reversed(created):
            cli('remove', type_, instance)
        cli('set', *(f'{r}={v}' for r, v in zip(refs, settings)))
        cli('save'); cli('reboot'); connect()
        assert cli('instances') == original
        assert cli('get', *refs)['typed_values'] == settings
        print('PASS motor smoke restored original layout and settings')
    if report:
        Path(report).write_text(json.dumps({'pass': True, 'endpoint': endpoint, 'trace': trace}, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint', default='serial://COM3')
    ap.add_argument('--modules', choices=['mock', 'real'], default='mock')
    ap.add_argument('--loop-rate', type=int, default=50)
    ap.add_argument('--report')
    ap.add_argument('--stack-wait', type=float, default=0)
    args = ap.parse_args()
    if args.modules == 'real':
        ap.error('real module/pin setup is scheduled in F5a-2; no configuration was changed')
    run(args.cli, args.endpoint, args.loop_rate, args.report, args.stack_wait)


if __name__ == '__main__':
    main()
