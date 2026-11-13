"""Run the cascade controller against mocks (vdev or bare board) or real modules.

--modules mock (default): controller.cascade_pid with actuator.mock and
feedback.mock, over TCP (vdev) or UART (bare board).

--modules real: actuator.pwm_dir and feedback.quadrature on real pins, UART
only. Two forms:
  --no-motor     bare bench board, nothing attached: the modules start on real
                 pins, the hardware force follows arm, disarm and the watchdog,
                 the duty cap holds and the count stays still. --pool-check
                 also proves the MCPWM operator and PCNT unit limits.
  (default)      a real motor and encoder. First-run procedure, every time:
                 wheels off the ground, a current-limited supply, a low duty
                 cap (--duty-cap, default 0.2) and a hand on the power switch.
                 It needs --confirm-first-run. It checks the encoder polarity
                 before any closed loop, then velocity and position tracking,
                 and that the watchdog removes physical effort. Never infer
                 physical stopping from a zero command alone.

--stall (any form): stall the control loop while it drives (velocity mode
with mocks or a motor, open loop with --no-motor) for --stall-ms (500-4000).
Status must report Fault/LoopStalled before the loop has counted the stall's
overruns, so the safety monitor, not the loop, disarmed (device data: frames
can be read late); after it the controller and applied
duty are 0 (and `forced` is set on actuator.pwm_dir), and ACK_FAULT and re-ARM
work. The duty and `forced` signals are published by the loop, so only a motor
shows that the monitor's force stopped the output during the stall: the
encoder may move at most 25% of what it would at the pre-stall speed. (The
native test in test_motor_hardware.cpp proves the force without any tick.)
It needs the bench stall: a firmware built with CONFIG_CORE_BENCH_CRASH=y
(flash the normal firmware afterwards), or a vdev started with --allow-crash.
It is checked last in the preflight, while disarmed, before any change.

Every form refuses pending edits or configured groups, and restores the
original loops-only layout and settings.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import time

PID, MOCK_ACT, MOCK_FB, PWM_DIR, QUAD = 0x20, 112, 113, 0x21, 0x22
# Bench pins for the extra pool-check instances. They avoid the strapping pins
# (0, 3, 45, 46), the legacy IMU's I2C (8, 9), possible RGB LEDs (38, 48),
# USB (19, 20), PSRAM (26-37) and the protocol UART (43, 44).
POOL_PINS = [1, 2, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 39, 40, 41, 42, 47]


class Device:
    def __init__(self, cli_path, endpoint):
        self.cli_path, self.endpoint = cli_path, endpoint

    def __call__(self, *command, code=0):
        for attempt in range(10):
            p = subprocess.run([str(self.cli_path), '--endpoint', self.endpoint, '--json', *map(str, command)],
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

    def frames(self, command, count):
        """`command COUNT` (status, events): one JSON envelope per frame."""
        p = subprocess.run([str(self.cli_path), '--endpoint', self.endpoint, '--json', command, str(count)],
                           capture_output=True, text=True, timeout=20 + count)
        assert p.returncode == 0, (command, p.returncode, p.stdout, p.stderr)
        out = [json.loads(line) for line in p.stdout.splitlines() if line.strip()]
        assert all(r['ok'] for r in out), out
        return [r.get('data', r) for r in out]

    def refused(self, *command):
        """Run a command that may be refused (exit 5); True when it was."""
        try:
            self(*command)
            return False
        except AssertionError:
            self(*command, code=5)
            return True

    def connect(self):
        deadline = time.monotonic() + 15
        while True:
            try:
                return self('info')
            except (AssertionError, json.JSONDecodeError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.3)

    def reboot(self):
        self('save'); self('reboot'); return self.connect()


def preflight(cli, stall_ms=0):
    info = cli.connect()
    assert not info['flags'] & 11, 'pending edits, invalid layout or safe mode'
    status = cli('status')
    assert not status['flags'] & 8, 'unsaved params'
    original = cli('instances')
    assert all(x['type'] == 5 for x in original), 'requires an original loops-only layout'
    assert all(not g['configured'] for g in cli('groups')['groups']), 'requires idle groups'
    # Last, and only while disarmed: the monitor watches loops only while
    # armed, so a disarmed 10 ms stall changes nothing.
    if stall_ms:
        assert status['arm_state'] == 0, 'requires a disarmed device'
        if cli.refused('bench-stall', 0, 10):
            sys.exit('motor_smoke.py: error: --stall needs the bench stall: a CONFIG_CORE_BENCH_CRASH=y '
                     'firmware or a vdev with --allow-crash; no configuration was changed')
    refs = [f"5:{loop['id']}:1" for loop in info['loops']] + ['0:0:4']
    return info, original, refs, cli('get', *refs)['typed_values']


def restore(cli, created, original, refs, settings):
    cli('disarm')
    cli('cmd-clear', 0); cli('sub-clear', 0)
    for type_, instance in reversed(created):
        cli('remove', type_, instance)
    cli('set', *(f'{r}={v}' for r, v in zip(refs, settings)))
    cli.reboot()
    assert cli('instances') == original
    assert cli('get', *refs)['typed_values'] == settings
    print('PASS motor smoke restored original layout and settings')


def set_rates(cli, info, loop_rate):
    rates = [f"5:{loop['id']}:1={loop_rate if loop['id'] == 0 else (25 if cli.endpoint.startswith('tcp://') else loop['rate_hz'])}"
             for loop in info['loops']]
    cli('set', *rates, '0:0:4=1000')


class Stream:
    """Command group 0 drives the controller; subscription 0 samples it."""

    def __init__(self, cli, pid, extra, loop_rate, trace):
        self.cli, self.counter, self.trace = cli, 0, trace
        cli('cmd-config', 0, 0, f'{pid}:514', f'{pid}:512', f'{pid}:513')
        cli('sub-config', 0, 0, max(1, loop_rate // 50), *(f'{pid}:{i}' for i in range(256, 261)), *extra)

    def command(self, values=None):
        self.cli('cmd-send', 0, self.counter & 65535, *(values or [])); self.counter += 1

    def sample(self):
        s = self.cli('sub-stream', 0)['typed_values']
        self.trace.append(s)
        return s

    def drive(self, values, duration, check=None):
        end = time.monotonic() + duration
        samples = []
        while time.monotonic() < end:
            self.command(values); s = self.sample(); samples.append(s)
            if check:
                check(s)
            time.sleep(.02)
        return samples

    def wait_watchdog(self):
        # No receipt, including a keepalive, follows: the watchdog must stop.
        time.sleep(1.2)
        deadline = time.monotonic() + 2
        observed = []
        # Linux integration retains a PTY with buffered status across CLI
        # sessions. Its first frame can predate the deadline; native tests
        # verify timing, while this smoke waits for the actual state/reason.
        while True:
            status = self.cli('status'); observed.append(status)
            if status['arm_state'] != 1 or time.monotonic() >= deadline:
                break
            time.sleep(.05)
        assert status['arm_state'] == 0 and status['last_disarm_reason'] == 2, observed


def loop0_overruns(status):
    return next(o['overruns'] for o in status['overruns'] if o['loop_id'] == 0)


def stall_check(cli, io, values, stall_ms, loop_rate, real=False, motor=None):
    """Stall control loop 0 while armed and driving `values`.

    Samples are (pos, vel, duty, ...); index 5 is the applied duty, and with
    real modules 6 is `forced` and 7 the encoder count. `motor` is the counts
    per output radian for the encoder check, None without a motor.

    The proof uses device data, not host timing: a status frame can be read
    long after it was sent (the serial port keeps a backlog between CLI
    processes). A stalled tick counts its missed periods only when it ends,
    so a Fault/LoopStalled frame whose loop-0 overrun count is still below
    the jump the stall then adds was sent while the loop was stalled: the
    monitor disarmed, not the loop.
    """
    # One CLI call between watchdog feeds, as in Stream.drive: on the CH343
    # two calls can outlast the 1 s watchdog.
    end = time.monotonic() + .3
    while True:
        io.command(values)
        sampled_at = time.monotonic()
        before = io.sample()
        if time.monotonic() >= end:
            break
        time.sleep(.02)
    assert before[2] != 0, ('the stall check needs effort before the stall', before)
    io.command(values)
    cli('bench-stall', 0, stall_ms)
    replied = time.monotonic()
    jump = max(1, loop_rate * stall_ms // 2000)  # half the periods the stall misses
    frames, fault = [], None
    deadline = time.monotonic() + stall_ms / 1000 + 20
    while time.monotonic() < deadline:
        frames += cli.frames('status', 10)
        if fault is None:
            fault = next((f for f in frames if f['arm_state'] == 2), None)
        if fault is not None and loop0_overruns(frames[-1]) >= loop0_overruns(fault) + jump:
            break
    assert fault is not None, ('no Fault after the stall request', frames[-3:])
    assert fault['last_disarm_reason'] == 4, ('Fault, but not LoopStalled', fault)
    assert loop0_overruns(frames[-1]) >= loop0_overruns(fault) + jump, (
        'the stall\'s overruns were already counted when Fault was reported: not provably '
        'the monitor during the stall', fault, frames[-1])
    after, deadline = None, time.monotonic() + 5
    while time.monotonic() < deadline:  # skip samples queued from before the stall
        after = io.sample()
        if after[2] == 0 and after[5] == 0 and (not real or after[6] is True):
            break
    assert after[2] == 0 and after[5] == 0, ('controller and applied duty after the stall', after)
    if real:
        assert after[6] is True, ('forced after the stall', after)
    if motor is not None:
        # From the sample to the stall the motor ran for less than
        # `replied - sampled_at` (plus the sample's age, at most one 50 Hz
        # subscription period); without the monitor's force it would run on
        # for the whole stall.
        moved = abs(after[7] - before[7])
        speed = abs(before[1]) * motor
        allowed = speed * (replied - sampled_at + .05) + .25 * speed * stall_ms / 1000
        assert moved <= allowed, ('the encoder moved during the stall', moved, allowed, before, after)
    cli('ack-fault'); cli('arm'); io.command()
    s = io.sample(); assert s[2] == 0 and s[5] == 0, ('re-arm holds zero until a fresh command', s)
    if real:
        assert s[6] is False, ('re-arm releases the force', s)
    print(f'PASS loop stall: Fault/LoopStalled sent with loop-0 overruns at {loop0_overruns(fault)}, '
          f'before the stall added {loop0_overruns(frames[-1]) - loop0_overruns(fault)}; '
          'zero duty after it, ACK_FAULT and re-arm')


def run(cli_path, endpoint, loop_rate=50, report=None, stack_wait=0, stall_ms=0):
    cli = Device(cli_path, endpoint)
    info, original, refs, settings = preflight(cli, stall_ms)
    created, trace = [], []
    try:
        for type_, label in ((MOCK_ACT, 'motor_smoke'), (MOCK_FB, 'encoder_smoke'), (PID, 'pid_smoke')):
            created.append((type_, cli('add', type_, label, 0)['instance']))
        motor, encoder, controller = [i for _, i in created]
        pid = f'{PID}:{controller}'
        cli('set', f'{MOCK_FB}:{encoder}:1={(MOCK_ACT << 8) | motor}',
            f'{pid}:1={(MOCK_ACT << 8) | motor}', f'{pid}:2={(MOCK_FB << 8) | encoder}')
        cli.reboot()
        assert cli('get', f'{pid}:1', f'{pid}:2')['typed_values'] == [(MOCK_ACT << 8) | motor, (MOCK_FB << 8) | encoder]
        set_rates(cli, info, loop_rate)
        cli('set', f'{pid}:32=0.12', f'{pid}:33=0.8', f'{pid}:48=3', f'{pid}:49=2',
            f'{pid}:17=0.4', f'{pid}:18=4')
        cli.reboot()
        assert cli('get', f'{pid}:32', f'{pid}:49')['typed_values'][1] == 2
        descriptors = {x['name']: x for x in cli('items', PID)}
        assert descriptors['waiting_for_command']['value_type'] == 0
        io = Stream(cli, pid, [f'{MOCK_ACT}:{motor}:258'], loop_rate, trace)

        def bounded(s):
            assert abs(s[2]) <= .40001 and abs(s[3]) <= 4.00001, s

        cli('arm')
        io.drive([.2, 2, 0], .5, bounded)
        io.command()  # cover the mode-write round trip on the slower UART CLI path
        cli('set', f'{pid}:16=1')
        # Old velocity target was present in every open-loop frame. Keepalives
        # cannot turn it into a new-mode command.
        for _ in range(3):
            io.command(); assert io.sample()[4] is True
        velocity = io.drive([0, 2, 0], 6, bounded)
        assert abs(sum(s[1] for s in velocity[-5:])/len(velocity[-5:]) - 2) < .3, velocity[-5:]
        assert velocity[-1][4] is False
        if stall_ms:
            stall_check(cli, io, [0, 2, 0], stall_ms, loop_rate)
        io.command()
        cli('set', f'{pid}:16=2')
        io.command(); held = io.sample(); assert held[4] is True
        # Reuse the sample: another CLI discovery/stream round trip can leave
        # a >1 s gap between UART command receipts and correctly trip safety.
        target = held[0] + .5
        position = io.drive([0, 0, target], 8, bounded)
        assert abs(position[-1][0] - target) < .12, (target, position[-5:])
        assert abs(position[-1][1]) < .3, position[-5:]
        io.wait_watchdog()
        stopped = io.sample(); assert stopped[2] == 0 and stopped[5] == 0, stopped
        cli('arm'); io.command(); assert io.sample()[2] == 0
        cli('disarm')
        if stack_wait:
            time.sleep(stack_wait)
        print('PASS cascade velocity/position, mode freshness, persistence, watchdog and re-arm')
    finally:
        restore(cli, created, original, refs, settings)
    if report:
        Path(report).write_text(json.dumps({'pass': True, 'endpoint': endpoint, 'trace': trace}, indent=2))


def setup_real(cli, a, created):
    """Add the real modules and a controller; set pins, scaling, limits and gains.

    Each instance joins `created` as soon as it is added, so a later failure
    (a pin the device refuses, say) still restores the original layout.
    """
    for type_, label in ((PWM_DIR, 'motor_real'), (QUAD, 'encoder_real'), (PID, 'pid_real')):
        created.append((type_, cli('add', type_, label, 0)['instance']))
    motor, encoder, controller = [i for _, i in created]
    pid = f'{PID}:{controller}'
    flag = lambda value: 'true' if value else 'false'  # noqa: E731
    cli('set', f'{PWM_DIR}:{motor}:1={a.pwm_pin}', f'{PWM_DIR}:{motor}:2={a.dir_pin}',
        f'{PWM_DIR}:{motor}:3={flag(a.invert_motor)}', f'{PWM_DIR}:{motor}:4={a.duty_cap}',
        f'{QUAD}:{encoder}:1={a.enc_a}', f'{QUAD}:{encoder}:2={a.enc_b}',
        f'{QUAD}:{encoder}:3={flag(not a.no_pullup)}', f'{QUAD}:{encoder}:4={a.cpr}',
        f'{QUAD}:{encoder}:5={a.gear}', f'{QUAD}:{encoder}:7={flag(a.invert_encoder)}',
        f'{pid}:1={(PWM_DIR << 8) | motor}', f'{pid}:2={(QUAD << 8) | encoder}',
        f'{pid}:17={a.duty_cap}', f'{pid}:18={a.velocity_limit}',
        f'{pid}:32={a.vel_kp}', f'{pid}:33={a.vel_ki}', f'{pid}:48={a.pos_kp}', f'{pid}:49={a.pos_ki}')
    return motor, encoder, pid


def run_real(cli_path, endpoint, a):
    cli = Device(cli_path, endpoint)
    # Read-only, before any change: a vdev (TCP or a serial PTY) has no pins
    # and does not register the hardware types. The type list cannot tell: it
    # describes every generated type, registered or not.
    if cli.connect()['firmware_version'].endswith('-vdev'):
        sys.exit('motor_smoke.py: error: the device is a vdev, which has no pins and no '
                 'actuator.pwm_dir/feedback.quadrature; no configuration was changed')
    info, original, refs, settings = preflight(cli, a.stall_ms if a.stall else 0)
    created, trace = [], []
    try:
        motor, encoder, pid = setup_real(cli, a, created)
        set_rates(cli, info, a.loop_rate)
        cli.reboot()
        states = {(x['type'], x['instance']): x['state'] for x in cli('instances')}
        assert all(states[c] == 0 for c in created), ('real modules failed to start; see COM4', states)
        assert cli('get', f'{PWM_DIR}:{motor}:1', f'{QUAD}:{encoder}:2')['typed_values'] == [a.pwm_pin, a.enc_b]
        # Samples: pos, vel, duty, velocity target, waiting, applied duty, forced,
        # count, then the control loop's max_run_us and overruns.
        io = Stream(cli, pid, [f'{PWM_DIR}:{motor}:256', f'{PWM_DIR}:{motor}:257', f'{QUAD}:{encoder}:258',
                               '5:0:259', '5:0:257'], a.loop_rate, trace)
        cap = a.duty_cap + 1e-5

        def bounded(s):
            assert abs(s[2]) <= cap and abs(s[5]) <= cap, s

        s = io.sample(); assert s[6] is True and s[5] == 0, ('forced while disarmed', s)
        cli('arm')
        if a.no_motor:
            run_no_motor(cli, io, a, bounded)
        else:
            run_motor(cli, io, a, pid, bounded)
        io.wait_watchdog()
        stopped = io.sample()
        assert stopped[6] is True and stopped[5] == 0 and stopped[2] == 0, ('watchdog force', stopped)
        if not a.no_motor:
            # Physical effort is gone only if the encoder stops moving.
            time.sleep(1)
            first = io.sample()[7]; time.sleep(.5); last = io.sample()[7]
            assert abs(last - first) <= 2, ('the encoder still moves after the watchdog stop', first, last)
        cli('arm'); io.command()
        s = io.sample(); assert s[6] is False and s[5] == 0, ('re-arm releases at zero duty', s)
        cli('disarm')
        s = io.sample(); assert s[6] is True and s[5] == 0, ('disarm forces', s)
        print('PASS real PWM+DIR/quadrature: start on pins, force/release, duty cap, watchdog, re-arm'
              + (' (no motor)' if a.no_motor else ', polarity, velocity and position tracking'))
        print(f'control loop at {a.loop_rate} Hz: longest tick {s[8]} us, {s[9]} overruns')
        if a.stack_wait:
            time.sleep(a.stack_wait)  # hold the real layout for console stack sampling
    finally:
        restore(cli, created, original, refs, settings)
    if a.pool_check:
        pool_check(cli, a)
    if a.report:
        Path(a.report).write_text(json.dumps({'pass': True, 'endpoint': endpoint, 'trace': trace}, indent=2))


def run_no_motor(cli, io, a, bounded):
    # Over the cap: the controller and the actuator both hold the cap.
    samples = io.drive([1, 0, 0], .5, bounded)
    s = samples[-1]
    assert s[6] is False and abs(s[5] - a.duty_cap) < 1e-5, ('released and capped', s)
    if not a.no_pullup:
        assert s[7] == 0, ('nothing attached: the pulled-up encoder pins must not count', s)
    if a.stall:
        stall_check(cli, io, [1, 0, 0], a.stall_ms, a.loop_rate, real=True)
    samples = io.drive([-1, 0, 0], .5, bounded)  # reversal passes through one zero tick
    assert abs(samples[-1][5] + a.duty_cap) < 1e-5, samples[-1]
    io.drive([0, 0, 0], .2, bounded)


def run_motor(cli, io, a, pid, bounded):
    # Polarity first, in open loop at a small duty: positive duty must count up.
    # A reversed encoder would make every closed loop run away.
    start = io.sample()[7]
    io.drive([a.probe_duty, 0, 0], .6, bounded)
    moved = io.sample()[7] - start
    io.drive([0, 0, 0], .4, bounded)
    assert moved > 0, (f'positive duty moved the encoder by {moved} counts: check wiring, '
                       'or set --invert-encoder (or --invert-motor); nothing ran closed-loop')
    io.command()
    cli('set', f'{pid}:16=1')
    for _ in range(3):
        io.command(); assert io.sample()[4] is True
    velocity = io.drive([0, a.velocity_target, 0], 6, bounded)
    mean = sum(s[1] for s in velocity[-5:]) / len(velocity[-5:])
    assert abs(mean - a.velocity_target) < a.velocity_tolerance, velocity[-5:]
    if a.stall:
        stall_check(cli, io, [0, a.velocity_target, 0], a.stall_ms, a.loop_rate, real=True,
                    motor=a.cpr * a.gear / (2 * math.pi))
    io.command()
    cli('set', f'{pid}:16=2')
    io.command(); held = io.sample(); assert held[4] is True
    target = held[0] + a.position_step
    position = io.drive([0, 0, target], 8, bounded)
    assert abs(position[-1][0] - target) < a.position_tolerance, (target, position[-5:])


def pool_check(cli, a):
    """A 5th encoder and a 7th actuator fail start, and ARM is refused for it.

    The real modules of the main run are removed by then, so every pool pin
    is free (7 actuators need 14 of them).
    """
    for type_, count, label, first_param in ((QUAD, 5, 'enc', 1), (PWM_DIR, 7, 'act', 1)):
        info, original, refs, settings = preflight(cli)
        created = []
        try:
            free = iter(POOL_PINS)
            for n in range(count):
                instance = cli('add', type_, f'{label}{n}', 0)['instance']
                created.append((type_, instance))
                cli('set', f'{type_}:{instance}:{first_param}={next(free)}', f'{type_}:{instance}:{first_param + 1}={next(free)}')
            cli.reboot()
            states = [x['state'] for x in cli('instances') if x['type'] == type_]
            assert sorted(states) == [0] * (count - 1) + [1], (type_, states)
            # An empty command group satisfies the group condition, so the
            # refusal can only be the failed start (Fault/StartFailed).
            cli('cmd-config', 0, 0)
            refused = cli('arm', code=5)
            assert refused['result'] == 9 and refused['detail'] == 32, refused
            print(f'PASS pool limit: instance {count} of type {type_:#x} failed start and ARM was refused (StartFailed)')
            if a.stack_wait:
                time.sleep(a.stack_wait)  # the start-failure path is the deepest boot on main's stack
        finally:
            restore(cli, created, original, refs, settings)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint', default='serial://COM3')
    ap.add_argument('--modules', choices=['mock', 'real'], default='mock')
    ap.add_argument('--loop-rate', type=int, default=None, help='control loop rate (mock 50, real 1000)')
    ap.add_argument('--report')
    ap.add_argument('--stack-wait', type=float, default=0)
    ap.add_argument('--stall', action='store_true',
                    help='loop-stall scenario; needs a CONFIG_CORE_BENCH_CRASH=y build or vdev --allow-crash')
    ap.add_argument('--stall-ms', type=int, default=2000,
                    help='stall length, 500-4000 ms (below the 5 s Task WDT)')
    real = ap.add_argument_group('real modules (--modules real)')
    real.add_argument('--no-motor', action='store_true', help='bare bench board, nothing attached')
    real.add_argument('--pool-check', action='store_true', help='with --no-motor: MCPWM/PCNT pool limits')
    real.add_argument('--confirm-first-run', action='store_true',
                      help='wheels off the ground, current-limited supply, low duty cap, hand on the power switch')
    real.add_argument('--pwm-pin', type=int)
    real.add_argument('--dir-pin', type=int)
    real.add_argument('--enc-a', type=int)
    real.add_argument('--enc-b', type=int)
    real.add_argument('--cpr', type=float, default=1440, help='counts per motor revolution (4x decoded)')
    real.add_argument('--gear', type=float, default=1)
    real.add_argument('--invert-motor', action='store_true')
    real.add_argument('--invert-encoder', action='store_true')
    real.add_argument('--no-pullup', action='store_true')
    real.add_argument('--duty-cap', type=float, default=.2)
    real.add_argument('--probe-duty', type=float, default=.1, help='open-loop duty of the polarity check')
    real.add_argument('--velocity-limit', type=float, default=4)
    real.add_argument('--vel-kp', type=float, default=.02)
    real.add_argument('--vel-ki', type=float, default=.1)
    real.add_argument('--pos-kp', type=float, default=2)
    real.add_argument('--pos-ki', type=float, default=0)
    real.add_argument('--velocity-target', type=float, default=2, help='rad/s at the output shaft')
    real.add_argument('--velocity-tolerance', type=float, default=.5)
    real.add_argument('--position-step', type=float, default=1, help='rad at the output shaft')
    real.add_argument('--position-tolerance', type=float, default=.15)
    args = ap.parse_args()
    if not 500 <= args.stall_ms <= 4000:
        ap.error('--stall-ms must be 500-4000')
    if args.modules == 'mock':
        run(args.cli, args.endpoint, args.loop_rate or 50, args.report, args.stack_wait,
            args.stall_ms if args.stall else 0)
        return
    # Refuse before any connection or configuration change.
    if not args.endpoint.startswith('serial://'):
        ap.error('real modules need the device over serial (a vdev has no pins); no configuration was changed')
    if None in (args.pwm_pin, args.dir_pin, args.enc_a, args.enc_b):
        ap.error('--pwm-pin, --dir-pin, --enc-a and --enc-b are required')
    if not 0 < args.duty_cap <= 1 or not 0 < args.probe_duty <= args.duty_cap:
        ap.error('need 0 < --probe-duty <= --duty-cap <= 1')
    if args.pool_check and not args.no_motor:
        ap.error('--pool-check is a bare-board check: add --no-motor')
    if not args.no_motor and not args.confirm_first_run:
        ap.error('a motor run needs --confirm-first-run: wheels off the ground, a current-limited supply, '
                 'a low duty cap and a hand on the power switch')
    args.loop_rate = args.loop_rate or 1000
    run_real(args.cli, args.endpoint, args)


if __name__ == '__main__':
    main()
