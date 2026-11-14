"""Capture of mock signals and inputs through the CLI (F7a).

Runs on vdev (host integration) or the bare board (--endpoint serial://COM3).
Three actuator.mock instances go on the control loop. It checks a step
response captured around a threshold trigger on the commanded input, a manual
capture forced with TRIGGER at a divider (with the CaptureDone event), ABORT
and the refusals, and the longest control tick with a 1-ref and a 16-ref
capture. Requires a clean loops-only layout and idle groups; restores the
original saved layout and settings. Device timing only: sample spacing is
proven by the captured loop tick counter, never by host clocks.
"""
import argparse
import math
import sys
import time
from motor_smoke import Device, preflight, restore, set_rates

ACTUATOR = 112
DUTY, APPLIED, VELOCITY, POSITION = 512, 258, 256, 257
TICKS, LAST_RUN = 0x100, 0x102
WRONG_STATE = 46


def run(cli_path, endpoint, loop_rate=1000, expect_psram=False, stack_wait=0):
    cli = Device(cli_path, endpoint)
    info, original, refs, settings = preflight(cli)
    serial = endpoint.startswith('serial://')
    capacity = info['capture_capacity']
    assert capacity >= 32768, info
    if expect_psram:
        assert capacity > 32768, ('no PSRAM capture ring', capacity)
    limit = cli('get', '0:0:0x16')['values'][0]
    assert limit >= 16, limit
    created = []
    try:
        for i in range(3):
            created.append((ACTUATOR, cli('add', ACTUATOR, f'cap{i}', 0)['instance']))
        set_rates(cli, info, loop_rate)
        cli.reboot()
        a = [f'{ACTUATOR}:{inst}' for _, inst in created]
        loop = lambda item: f'5:0:{item}'
        status = cli('capture-status')
        assert status['state'] == 'idle' and not status['configured'], status

        def wait_recorded(n):
            # The pre-trigger history fills at the loop's real tick rate.
            deadline = time.monotonic() + 10
            while cli('capture-status')['recorded'] < n:
                assert time.monotonic() < deadline, 'pre-trigger history did not fill'
                time.sleep(.05)
        assert cli('capture-arm', code=5)['detail'] == WRONG_STATE

        # 1. Step response: threshold trigger on the commanded duty input.
        samples, pre = int(loop_rate * 1.5), loop_rate // 4
        step = [f'{a[0]}:{DUTY}', f'{a[0]}:{APPLIED}', f'{a[0]}:{VELOCITY}', loop(TICKS)]
        cli('cmd-config', 0, 0, f'{a[0]}:{DUTY}')
        cli('capture-config', 0, 1, samples, pre, 'threshold', f'{a[0]}:{DUTY}', 'rising', 0.25, *step)
        cli('capture-arm')
        assert cli('capture-config', 0, 1, 10, 0, 'manual', loop(TICKS), code=5)['detail'] == WRONG_STATE
        wait_recorded(pre)
        cli('cmd-run', 0, 20, 200, 0.5)  # ARM, 4 s of duty 0.5 at 50 Hz, DISARM
        done = cli('capture-wait', 5000)['status']
        data = cli('capture-read')
        assert done['recorded'] == samples and done['trigger_index'] == pre, done
        rows = [r['typed_values'] for r in data['rows']]
        ticks = [r[3] for r in rows]
        assert all(b - a_ == 1 for a_, b in zip(ticks, ticks[1:])), 'a control tick was not recorded'
        assert all(r[0] == 0 for r in rows[:pre]) and rows[pre][0] == 0.5, rows[pre - 2:pre + 2]
        assert any(r[1] == 0.5 for r in rows[pre:pre + 3]), rows[pre:pre + 3]
        peak = max(r[2] for r in rows[pre:])
        assert abs(rows[pre - 1][2]) < 1e-3 and peak > 3, (rows[pre - 1], peak)
        if serial:
            assert not done['irregular'], done
        print(f'PASS step response: {samples} samples at {loop_rate} Hz, trigger at {pre}, '
              f'peak velocity {peak:.3f}, irregular={done["irregular"]}')

        # 2. Manual capture at a divider, forced with TRIGGER; the done event.
        divider, samples, pre = max(1, loop_rate // 100), 200, 50
        cli('capture-config', 0, divider, samples, pre, 'manual', loop(TICKS))
        cli('capture-arm')
        assert cli('capture-read', code=2)
        wait_recorded(pre)
        cli('capture-trigger')
        waited = cli('capture-wait', 10000)
        assert waited['event'] and waited['event']['name'] == 'capture_done', waited
        assert waited['event']['recorded'] == samples and waited['event']['trigger_index'] == pre, waited
        ticks = [r['typed_values'][0] for r in cli('capture-read')['rows']]
        assert len(ticks) == samples and all(b - a_ == divider for a_, b in zip(ticks, ticks[1:])), ticks[:5]
        assert cli('capture-trigger', code=5)['detail'] == WRONG_STATE
        print(f'PASS manual TRIGGER capture: divider {divider}, pre {pre}, CaptureDone event')

        # 3. ABORT keeps the configuration and discards the recording.
        cli('capture-arm')
        cli('capture-abort')
        status = cli('capture-status')
        assert status['state'] == 'idle' and status['configured'] and status['recorded'] == 0, status
        print('PASS ABORT')

        # 4. The longest control tick with a 1-ref and a 16-ref capture.
        def longest(capture_refs):
            n = loop_rate * 2
            cli('capture-config', 0, 1, n, 0, 'manual', *capture_refs)
            cli('capture-arm'); cli('capture-trigger')
            status = cli('capture-wait', 10000)['status']
            column = capture_refs.index(loop(LAST_RUN))
            runs = [r['typed_values'][column] for r in cli('capture-read')['rows']]
            return max(runs[1:]), status['irregular']
        wide = [f'{x}:{item}' for x in a for item in (DUTY, APPLIED, VELOCITY, POSITION)]
        wide += [loop(i) for i in (TICKS, 0x101, LAST_RUN, 0x103)]
        assert len(wide) == 16
        base, base_irregular = longest([loop(LAST_RUN)])
        full, full_irregular = longest(wide)
        period = 1000000 // loop_rate
        print(f'Longest control tick at {loop_rate} Hz: {base} us with 1 ref, {full} us with 16 refs '
              f'(irregular {base_irregular}/{full_irregular})')
        if serial:
            assert full < period and not base_irregular and not full_irregular, (base, full)
        print('PASS capture tick cost')
        if stack_wait:
            cli('capture-arm')
            time.sleep(stack_wait)
            cli('capture-abort')
    finally:
        cli('capture-abort')
        restore(cli, created, original, refs, settings)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--loop-rate', type=int, default=1000)
    parser.add_argument('--expect-psram', action='store_true', help='require a PSRAM ring (more than 32 KB)')
    parser.add_argument('--stack-wait', type=float, default=0, help='hold an armed capture for console stack observation')
    args = parser.parse_args()
    if not 100 <= args.loop_rate <= 2000 or not math.isfinite(args.stack_wait) or not 0 <= args.stack_wait <= 3600:
        parser.error('--loop-rate must be 100..2000 and --stack-wait 0..3600 s')
    try:
        run(args.cli, args.endpoint, args.loop_rate, args.expect_psram, args.stack_wait)
    except (AssertionError, RuntimeError) as exc:
        sys.exit(f'capture_smoke.py: {exc}')
