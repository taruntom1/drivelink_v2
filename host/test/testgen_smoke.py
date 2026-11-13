"""F7b step/chirp captures on vdev or the bare board, using the real core.

Real motors: wheels off the ground, current-limited supply, low duty cap,
hand on the power switch. Requires --modules real --confirm-first-run and
explicit pins/scaling. The same workflow is dry-run with mocks by default.
Restores the original loops-only layout/settings; refuses pending edits/groups.
Timing assertions use captured device tick counters, never host arrival times.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import time
from motor_smoke import (Device, preflight, restore, set_rates, setup_real,
                         MOCK_ACT, MOCK_FB, PID)


def run(cli_path, endpoint, loop_rate=1000, real=None, stack_wait=0, report=None, flash_write=False):
    cli = Device(cli_path, endpoint)
    if real and cli.connect()['firmware_version'].endswith('-vdev'):
        raise AssertionError('real modules require a physical device; no configuration changed')
    info, original, refs, settings = preflight(cli)
    created, results = [], []
    amplitude = real.probe_duty if real else .1
    try:
        if real:
            _, _, pid = setup_real(cli, real, created)
        else:
            for type_, label in ((MOCK_ACT, 'tg_motor'), (MOCK_FB, 'tg_encoder'), (PID, 'tg_pid')):
                created.append((type_, cli('add', type_, label, 0)['instance']))
            motor, encoder, controller = [i for _, i in created]
            pid = f'{PID}:{controller}'
            cli('set', f'{MOCK_FB}:{encoder}:1={(MOCK_ACT << 8) | motor}',
                f'{pid}:1={(MOCK_ACT << 8) | motor}', f'{pid}:2={(MOCK_FB << 8) | encoder}',
                f'{pid}:17=0.2')
        set_rates(cli, info, loop_rate)
        cli.reboot()
        target = f'{pid}:514'
        assert all(x['state'] == 0 for x in cli('instances')), 'module startup failure'
        cli('cmd-config', 0, 0, target)
        assert cli('testgen-status', 0)['state'] == 'idle'
        cli('testgen-stop', 0)  # idempotent while idle/disarmed
        cli('testgen-wait', 0, 10, code=2)
        cli('testgen-start', 0, target, 'invalid', 0, amplitude, 0, 0, 1000, 1, code=2)
        cli('testgen-start', 0, target, 'step', 'nan', amplitude, 0, 0, 1000, 1, code=2)
        assert cli('testgen-start', 0, target, 'step', 0, amplitude, 0, 0, 1000, 1, code=5)['result'] == 7
        if endpoint.startswith('tcp://'):
            from testgen_cli_failure import run as lost_arm_reply
            lost_arm_reply(cli_path, endpoint, target)
        columns = [target, f'{pid}:258', f'{pid}:257', f'{pid}:256',
                   f'{pid}:260', '5:0:256', '5:0:258', '5:0:257']

        for waveform, f0, f1 in (('step', 0, 0), ('chirp_linear', .5, 3)):
            samples, pre = loop_rate * 3, loop_rate // 4
            cli('capture-config', 0, 1, samples, pre, 'testgen_start', *columns)
            cli('capture-arm')
            deadline = time.monotonic() + 10
            while cli('capture-status')['recorded'] < pre:
                assert time.monotonic() < deadline, 'pre-trigger history did not fill'
                time.sleep(.05)
            run_result = cli('testgen-run', 0, 20, 8000, 0, target, waveform,
                             0, amplitude, f0, f1, 2000, 1)
            assert run_result['status']['reason'] == 'completed', (run_result, cli('diag-entries'))
            assert not run_result['interrupted'], run_result
            done = cli('capture-wait', 10000)['status']
            data = cli('capture-read')
            rows = [r['typed_values'] for r in data['rows']]
            trigger = done['trigger_index']
            assert trigger is not None and trigger <= pre, done
            assert len(rows) == samples, (len(rows), samples)
            assert all(all(isinstance(v, bool) or math.isfinite(v) for v in row) for row in rows)
            active = rows[trigger:trigger + int(loop_rate * 1.8)]
            assert len(active) >= loop_rate, 'too little active recording'
            assert all(abs(r[0]) <= amplitude + 1e-5 for r in active)
            assert any(not r[4] for r in active), 'controller never accepted generated input'
            if waveform == 'step':
                assert abs(rows[trigger][0] - amplitude) < 1e-5, rows[trigger]
                assert max(r[2] for r in active) > (real.min_response if real else .3), 'positive step response/polarity failed'
            else:
                assert abs(rows[trigger][0]) < 1e-5, rows[trigger]
                assert min(r[0] for r in active) < -.5 * amplitude
                assert max(r[0] for r in active) > .5 * amplitude
                if not done['irregular']:
                    for i, row in enumerate(active):
                        t = i * done['period_us'] * 1e-6
                        expected = amplitude * math.sin(2 * math.pi * (.5 * t + .5 * (3 - .5) * t*t / 2))
                        assert abs(row[0] - expected) < max(.003, amplitude * .03), (i, row[0], expected)
            ticks = [r[5] for r in rows]
            if endpoint.startswith('serial://') and not info['firmware_version'].endswith('-vdev'):
                assert not done['irregular'], done
                assert all(b - a == 1 for a, b in zip(ticks, ticks[1:])), 'missing device tick'
                assert rows[-1][7] == rows[0][7], 'control-loop overruns'
                assert max(r[6] for r in rows[1:]) < done['period_us'], 'tick exceeded budget'
            result = {'waveform': waveform, 'samples': samples, 'trigger': trigger,
                      'irregular': done['irregular'], 'longest_tick_us': max(r[6] for r in rows[1:]),
                      'peak_velocity': max(r[2] for r in active)}
            results.append(result)
            print('PASS testgen capture: ' + json.dumps(result), flush=True)

        # Exercise the lower-level service without the run helper's automatic DISARM.
        cli('arm')
        cli('testgen-start', 0, target, 'step', 0, amplitude, 0, 0, 1000, 0)
        cli('cmd-send', 0, 1, -amplitude)  # ignored for target, still feeds watchdog
        cli('testgen-stop', 0)
        assert cli('testgen-status', 0)['reason'] == 'stopped'
        cli('cmd-send', 0, 2)  # prefix-only feeds keep UART process gaps below watchdog
        cli('capture-config', 0, 1, 10, 0, 'manual', target)
        cli('capture-arm'); cli('cmd-send', 0, 3); cli('capture-trigger')
        cli('capture-wait', 10000); cli('cmd-send', 0, 4)
        assert all(abs(r['typed_values'][0] - amplitude) < 1e-5 for r in cli('capture-read')['rows'])
        cli('cmd-send', 0, 5, 0)
        cli('capture-arm'); cli('capture-trigger'); cli('cmd-send', 0, 6)
        cli('capture-wait', 10000); cli('cmd-send', 0, 7)
        assert all(abs(r['typed_values'][0]) < 1e-5 for r in cli('capture-read')['rows'])
        cli('disarm')
        print('PASS testgen STOP holds last value, ignored command discarded, fresh command takes over', flush=True)

        # Start a long run without keepalives: generation cannot feed the watchdog.
        cli('arm'); cli('testgen-start', 0, target, 'step', 0, amplitude, 0, 0, 1000, 0)
        time.sleep(1.2)
        assert cli('testgen-wait', 0, 5000)['status']['reason'] == 'disarmed'
        cli('disarm')
        print('PASS testgen watchdog cancellation', flush=True)
        if flash_write:
            cli('capture-config', 0, 1, loop_rate * 3, 0, 'manual',
                '5:0:258', '5:0:256', '5:0:257')
            cli('capture-arm'); cli('capture-trigger')
            deadline = time.monotonic() + 10
            while cli('capture-status')['recorded'] < loop_rate // 2:
                assert time.monotonic() < deadline
                time.sleep(.05)
            cli('save')  # deliberately disarmed; existing armed restrictions stay
            done = cli('capture-wait', 10000)['status']
            rows = [r['typed_values'] for r in cli('capture-read')['rows']]
            result = {'flash_write': True, 'irregular': done['irregular'],
                      'longest_tick_us': max(r[0] for r in rows[1:]),
                      'overruns_added': rows[-1][2] - rows[0][2]}
            results.append(result)
            print('PASS disarmed SAVE measurement: ' + json.dumps(result), flush=True)
        if stack_wait:
            # A long run with keepalives allows simultaneous COM4 stack observation.
            cli('capture-abort')
            cli('capture-config', 0, 1, int(loop_rate * (stack_wait + 2)), 0, 'testgen_start', *columns)
            cli('capture-arm')
            remaining = stack_wait
            while remaining > 0:
                # Device bounds each CLI subprocess to 20 s. Repeated short
                # runs keep a long stack observation inside that bound.
                duration = min(10, remaining)
                cli('testgen-run', 0, 20, int((duration + 5) * 1000), 0, target, 'chirp_log',
                    0, amplitude, .5, 3, max(1, int(duration * 1000)), 1)
                remaining -= duration
    finally:
        cli('disarm'); cli('capture-abort')
        restore(cli, created, original, refs, settings)
    if report:
        Path(report).write_text(json.dumps({'pass': True, 'endpoint': endpoint, 'results': results}, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint', required=True)
    ap.add_argument('--loop-rate', type=int, default=1000)
    ap.add_argument('--stack-wait', type=float, default=0)
    ap.add_argument('--report')
    ap.add_argument('--flash-write', action='store_true', help='measure disarmed SAVE during capture for XiP comparison')
    ap.add_argument('--modules', choices=['mock', 'real'], default='mock')
    ap.add_argument('--confirm-first-run', action='store_true')
    for flag in ('pwm-pin', 'dir-pin', 'enc-a', 'enc-b'):
        ap.add_argument('--' + flag, type=int)
    for flag, default in (('cpr', 1920), ('gear', 1), ('duty-cap', .2), ('probe-duty', .1),
                          ('velocity-limit', 4), ('vel-kp', .02), ('vel-ki', .1),
                          ('pos-kp', 2), ('pos-ki', 0), ('min-response', .05)):
        ap.add_argument('--' + flag, type=float, default=default)
    for flag in ('invert-motor', 'invert-encoder', 'no-pullup'):
        ap.add_argument('--' + flag, action='store_true')
    args = ap.parse_args()
    if not 50 <= args.loop_rate <= 2000 or not math.isfinite(args.stack_wait) or not 0 <= args.stack_wait <= 120:
        ap.error('loop rate must be 50..2000 Hz, stack wait 0..120 s')
    if args.modules == 'real':
        if not args.endpoint.startswith('serial://') or not args.confirm_first_run:
            ap.error('real motor requires serial and --confirm-first-run: wheels raised, current-limited supply, low duty cap, power switch ready')
        if None in (args.pwm_pin, args.dir_pin, args.enc_a, args.enc_b):
            ap.error('real motor requires all four pins')
        values = (args.cpr, args.gear, args.duty_cap, args.probe_duty, args.min_response,
                  args.velocity_limit, args.vel_kp, args.vel_ki, args.pos_kp, args.pos_ki)
        if not all(math.isfinite(v) for v in values) or not (0 < args.probe_duty <= args.duty_cap <= .2) or min(args.cpr, args.gear, args.min_response) <= 0:
            ap.error('invalid motor settings; require 0 < probe duty <= duty cap <= 0.2, positive scaling/response')
    run(args.cli, args.endpoint, args.loop_rate, args if args.modules == 'real' else None,
        args.stack_wait, args.report, args.flash_write)


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, RuntimeError) as exc:
        sys.exit(f'testgen_smoke.py: {exc}')
