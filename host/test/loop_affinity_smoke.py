"""Loop placement workflow on vdev or a bare board; restores a clean loops-only layout.

--console COM4 checks stack marks; --bench additionally requires the bench-only
initial/effective affinity observations. The console is closed before cleanup
reboots. No motors, pins, IMU, or factory reset are used.
"""
import argparse
import json
from pathlib import Path
import re
import time

from motor_smoke import Device, preflight


def observe_console(port, seconds, expected, bench):
    import serial  # Only hardware console checks need pyserial.
    chunks = []
    with serial.Serial(port, 115200, timeout=.2) as console:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chunks.append(console.read(max(1, console.in_waiting)))
    text = b''.join(chunks).decode(errors='replace')
    stacks = {name: int(free) for name, free in re.findall(
        r'Stack high-water:\s+(\S+)\s+(\d+) bytes free', text)}
    names = {'main', 'uart_rx', 'uart_tx', 'diag', 'esp_timer', 'core_cfg'} | set(expected)
    assert names <= stacks.keys(), ('missing stack observations', names - stacks.keys(), text)
    assert all(stacks[name] > 1024 for name in names), stacks
    observations = {}
    if bench:
        pattern = (r'Bench affinity: (\S+) requested=(-?\d+) initial=(-?\d+) '
                   r'initial_cpu=(-?\d+) after=(-?\d+) after_cpu=(-?\d+)')
        observations = {name: list(map(int, values)) for name, *values in re.findall(pattern, text)}
        assert expected.keys() <= observations.keys(), ('missing affinity observations', observations, text)
        for name, requested in expected.items():
            request, initial, initial_cpu, after, after_cpu = observations[name]
            assert request == initial == requested, (name, observations[name])
            assert initial_cpu in (0, 1) and after in (0, 1) and after_cpu == after
            assert requested == -1 or after == requested, (name, observations[name])
    return {'stacks': stacks, 'affinities': observations, 'console': text}


def run(cli_path, endpoint, console=None, stack_seconds=35, bench=False, report=None):
    cli = Device(cli_path, endpoint)
    info, original, _, _ = preflight(cli)
    assert cli('status')['arm_state'] == 0, 'requires a disarmed device'
    capture = cli('capture-status')
    assert capture['state'] == 'idle' and not capture['configured'], 'requires idle, unconfigured capture'
    assert not cli('status')['flags'] & 16, 'testgen is active'
    assert cli('get', '0:0:16')['typed_values'][0] >= len(info['loops']) + 2, 'needs two free loop slots'
    item = next(i for i in cli('items', 5) if i['name'] == 'core')
    assert item['value_type'] == 6 and item['default_bits'] == 2 and item['flags'] & 1, item
    assert cli('enums', 5, item['id']) == [
        {'name': 'core_0', 'value': 0}, {'name': 'core_1', 'value': 1}, {'name': 'any', 'value': 2}]
    core_refs = [f"5:{loop['id']}:3" for loop in info['loops']]
    original_cores = cli('get', *core_refs)['typed_values'] if core_refs else []
    created = []
    evidence = {'original_cores': original_cores}
    try:
        loops = []
        for realtime, module in ((True, 112), (False, 114)):
            loop = cli('add', 5, 'aff_rt' if realtime else 'aff_bg')['instance']
            created.append((5, loop)); loops.append(loop)
            ref = f'5:{loop}:3'
            assert cli('get-pending', ref)['typed_values'] == [2]
            for choice, value in (('core_0', 0), ('core_1', 1), ('0', 0), ('1', 1), ('any', 2), ('2', 2)):
                assert cli('set', f'{ref}={choice}')['pending_reboot']
                assert cli('get-pending', ref)['typed_values'] == [value]
            for invalid in ('3', '255', '256', 'unknown'):
                cli('set', f'{ref}={invalid}', code=2)  # rejected by discovered CLI enum validation
                assert cli('get-pending', ref)['typed_values'] == [2]
            cli('set', f'5:{loop}:1=50', f'5:{loop}:2={str(realtime).lower()}')
            instance = cli('add', module, 'aff_motor' if realtime else 'aff_imu', loop)['instance']
            created.append((module, instance))
        cli.reboot()
        assert cli('get', *core_refs)['typed_values'] == original_cores if core_refs else True
        for loop in loops:
            assert cli('get', f'5:{loop}:3')['typed_values'] == [2]
            assert next(l for l in cli('info')['loops'] if l['id'] == loop)['rate_hz'] == 50
        assert all(x['state'] == 0 for x in cli('instances')), cli('instances')
        for group, loop in enumerate(loops):
            cli('sub-config', group, loop, 1, *(f'5:{loop}:{signal}' for signal in (256, 257, 258, 259)))
        def sample():
            return [value for group in range(2) for value in cli('sub-stream', group)['typed_values']]
        before = sample()
        time.sleep(.15)
        after = sample()
        assert all(after[i] > before[i] for i in (0, 4)), (before, after)
        evidence['loop_stats'] = dict(zip(map(str, loops), [after[:4], after[4:]]))
        evidence['diagnostics'] = cli('diag-entries')
        # Discard rolls back reboot-required edits. REVERT retains them.
        ref = f'5:{loops[0]}:3'
        cli('set', f'{ref}=core_0')
        assert cli('get', ref)['typed_values'] == [2]
        assert cli('get-pending', ref)['typed_values'] == [0]
        cli('revert')
        assert cli('get-pending', ref)['typed_values'] == [0]
        cli('discard')
        assert cli('get', ref)['typed_values'] == [2]
        cli('sub-clear', 0); cli('sub-clear', 1)
        # SAVE/reboot the final selection a second time to prove persistence.
        cli.reboot()
        assert cli('get', *(f'5:{loop}:3' for loop in loops))['typed_values'] == [2, 2]
        if console:
            expected = {f"core_loop_{loop['id']}": (-1 if value == 2 else value)
                        for loop, value in zip(info['loops'], original_cores)}
            expected.update({f'core_loop_{loop}': -1 for loop in loops})
            evidence.update(observe_console(console, stack_seconds, expected, bench))
        print('PASS loop placement enum discovery, new default, choices, invalid values, pending edits, ticking and persistence', flush=True)
        print('Loop stats (ticks, overruns, last/max us):', evidence['loop_stats'], flush=True)
    finally:
        # Include every entry as soon as ADD succeeds, so partial setup cleans up.
        cli('disarm')
        cli('discard')
        running = {(x['type'], x['instance']) for x in cli('instances')}
        for type_, instance in reversed(created):
            if (type_, instance) in running:
                cli('remove', type_, instance)
        cli.reboot()
        assert cli('instances') == original
        assert cli('get', *core_refs)['typed_values'] == original_cores if core_refs else True
        print('PASS loop placement restored original layout and core selections', flush=True)
    evidence['pass'] = True
    if report:
        Path(report).write_text(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint', required=True)
    ap.add_argument('--console')
    ap.add_argument('--bench', action='store_true')
    ap.add_argument('--stack-seconds', type=float, default=35)
    ap.add_argument('--report')
    args = ap.parse_args()
    assert not args.bench or args.console, '--bench requires --console'
    run(args.cli, args.endpoint, args.console, args.stack_seconds, args.bench, args.report)
