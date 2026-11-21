#!/usr/bin/env python3
"""H4a deferred hardware check: DriveLink state through the real ros2_control stack.

Runs the drivelink_hardware plugin (drivelink_hardware/DriveLinkSystem) inside
`ros2_control_node`, with joint_state_broadcaster and imu_sensor_broadcaster,
against a real ESP behind drivelink-agent on the Pi, and checks what ROS sees.
State interfaces only: nothing is commanded, armed or written. The plugin takes
the agent's lease while active (that is the only thing it changes).

Checks, in order (each printed as PASS/FAIL with the numbers):
  1 streaming  /joint_states >= 80 % of --state-rate over 3 s, every joint
               present, finite; IMU messages >= 80 %, finite, |q| within 0.02 of 1
  2 IMU rest   |linear_acceleration| within 0.5 of 9.81 m/s^2 and
               |angular_velocity| < 0.05 rad/s over 2 s (robot kept still)
  3 wheels     (real) per joint: one full revolution FORWARD by hand moves the
               position by +2*pi within 15 %; held still, |velocity| < 0.05 rad/s
  4 IMU tilt   (real) nose up by about 30 deg: pitch changes by 20..40 deg with
               the sign REP 103 expects (nose up is negative pitch about +Y)
  5 active     the controller_manager reports the "drivelink" hardware
               component active, i.e. the plugin configured, took the lease
               and subscribed its groups

Run it in a shell where ROS 2 and the workspace install space are sourced
(`source /opt/ros/$ROS_DISTRO/setup.bash; source INSTALL/setup.bash`); it needs
`ros2` and the package drivelink_hardware (`ros2 pkg prefix drivelink_hardware`).
It starts its own ros2_control_node and spawners, so stop anything else that
already drives the device (the plugin refuses a lease held by another client).

Real hardware (the robot on stands, wheels free, ESP behind the agent):

  python3 host/test/ros_state_check.py --agent tcp://127.0.0.1:47620 \\
      --device base --token-file /etc/drivelink/token \\
      --joint left_wheel=left --joint right_wheel=right --imu imu=imu \\
      --state-rate 100 --report ros-state-check.json

First run: nothing is commanded (state only), but the robot must still be on
stands: you will be turning wheels by hand next to it and tilting it. Joint
labels (--joint NAME=LABEL, e.g. left_wheel=left) and the IMU label are the
instance labels of the device layout; each joint exports position and
velocity, the IMU its orientation, angular velocity and linear acceleration.

Pass means exit 0 and a PASS (or SKIPPED) line for every check. A negative
revolution means the encoder polarity is inverted (fix the layout or the
URDF, not this script).

--mock needs no hardware: it starts drivelink-vdev behind drivelink-agent
with the example layout (controllers "left" and "right" on mock actuators and
feedback, imu.mock "imu"), runs checks 1, 2 and 5 and skips 3 and 4:

  python3 host/test/ros_state_check.py --mock --vdev V --agent-bin A --cli C
"""
import argparse
import json
import math
import os
from pathlib import Path
import queue
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

PLUGIN = 'drivelink_hardware/DriveLinkSystem'
HW_NAME = 'drivelink'
IMU_TOPIC = '/imu_sensor_broadcaster/imu'
IMU_INTERFACES = ['orientation.x', 'orientation.y', 'orientation.z', 'orientation.w',
                  'angular_velocity.x', 'angular_velocity.y', 'angular_velocity.z',
                  'linear_acceleration.x', 'linear_acceleration.y', 'linear_acceleration.z']
GRAVITY = 9.81
PROCS = []
RESULTS = []


class Fail(Exception):
    pass


# ----------------------------------------------------------------- processes

class Proc:
    """A child in its own process group, with stdout/stderr kept for diagnostics."""

    def __init__(self, name, argv, env=None):
        self.name, self.argv = name, argv
        self.lines, self._q = [], queue.Queue()
        self.p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                  env=env, start_new_session=True)
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()
        PROCS.append(self)

    def _pump(self):
        for line in self.p.stdout:
            line = line.rstrip('\n')
            self.lines.append(line)
            self._q.put(line)

    def next_line(self, timeout, prefix=''):
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self._q.get(timeout=0.2)
                if line.startswith(prefix):
                    return line
            except queue.Empty:
                pass
            if self.p.poll() is not None and self._q.empty():
                raise Fail(f'{self.name} exited ({self.p.returncode}) before printing {prefix!r}:\n' + self.tail())
            if time.monotonic() >= deadline:
                raise Fail(f'{self.name}: no {prefix!r} line within {timeout} s:\n' + self.tail())

    def tail(self, n=25):
        return '\n'.join('    ' + l for l in self.lines[-n:])

    def alive(self):
        return self.p.poll() is None

    def _signal(self, sig):
        try:
            os.killpg(self.p.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass

    def stop(self, int_wait=8, term_wait=5):
        """SIGINT, then SIGTERM, then SIGKILL to the whole process group, bounded."""
        for sig, wait in ((signal.SIGINT, int_wait), (signal.SIGTERM, term_wait), (signal.SIGKILL, 5)):
            self._signal(sig)
            deadline = time.monotonic() + wait
            while time.monotonic() < deadline:
                if self.p.poll() is not None:
                    break
                time.sleep(0.05)
            if self.p.poll() is not None:
                break
        # a leader that exited may leave group members behind
        self._signal(signal.SIGKILL)
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        self._thread.join(timeout=2)


def stop_all():
    for proc in reversed(PROCS):
        proc.stop()
    PROCS.clear()


# ----------------------------------------------------------------- reporting

def report(name, verdict, text, **values):
    """verdict: PASS, FAIL or SKIPPED."""
    print(f'{verdict:7} {name}: {text}', flush=True)
    RESULTS.append({'check': name, 'verdict': verdict, 'detail': text, 'values': values})
    return verdict != 'FAIL'


def finite(x):
    return isinstance(x, float) and math.isfinite(x)


# ------------------------------------------------------------- mock topology

def mock_cli(cli, endpoint, *command, code=0):
    run = subprocess.run([cli, '--endpoint', endpoint, '--json', *map(str, command)],
                         capture_output=True, text=True, timeout=30)
    try:
        envelope = json.loads(run.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        raise Fail(f'cli {command}: no JSON on stdout (exit {run.returncode}): {run.stderr}')
    if run.returncode != code or envelope.get('ok') != (code == 0):
        raise Fail(f'cli {command}: exit {run.returncode}: {run.stdout} {run.stderr}')
    return envelope.get('data', envelope)


def mock_reboot(cli, endpoint):
    try:
        mock_cli(cli, endpoint, 'reboot')
    except Fail:
        pass
    deadline = time.monotonic() + 20
    while True:
        time.sleep(0.4)
        try:
            return mock_cli(cli, endpoint, 'info')
        except Fail:
            if time.monotonic() > deadline:
                raise Fail('the vdev did not come back after the reboot')


def start_mock(args, tmp, token, *, gains=None, scenario=None):
    """vdev with the example layout behind drivelink-agent. Returns (agent url, alias)."""
    for name in ('vdev', 'agent_bin', 'cli'):
        path = getattr(args, name)
        if not path or not os.path.isfile(path):
            raise Fail(f'--mock needs --{name.replace("_", "-")} (or DRIVELINK_{name.split("_")[0].upper()}): {path!r}')
    argv = [args.vdev, '--port', '0', '--state', str(tmp / 'vdev')]
    if scenario is not None:
        argv += ['--scenario-file', str(scenario)]
    vdev = Proc('vdev', argv)
    endpoint = vdev.next_line(15, 'READY ')[len('READY '):]
    info = mock_cli(args.cli, endpoint, 'info')
    control = next(loop for loop in info['loops'] if loop['realtime'])
    sensor = next(loop for loop in info['loops'] if not loop['realtime'])
    # Example layout: per side an actuator.mock (112), a feedback.mock (113) and a
    # controller.cascade_pid (32) on the control loop; one imu.mock (114) on the sensor loop.
    act, fb, pid = 112, 113, 32
    made = {}
    for side in ('left', 'right'):
        made[side] = {kind: mock_cli(args.cli, endpoint, 'add', t, f'{label}', control['id'])['instance']
                      for kind, t, label in (('act', act, f'{side}_act'), ('fb', fb, f'{side}_enc'))}
        made[side]['pid'] = mock_cli(args.cli, endpoint, 'add', pid, side, control['id'])['instance']
    imu = mock_cli(args.cli, endpoint, 'add', 114, 'imu', sensor['id'])['instance']
    sets = []
    for side, m in made.items():
        sets += [f'{fb}:{m["fb"]}:1={(act << 8) | m["act"]}',
                 f'{pid}:{m["pid"]}:1={(act << 8) | m["act"]}', f'{pid}:{m["pid"]}:2={(fb << 8) | m["fb"]}']
        if gains:
            sets += [f'{pid}:{m["pid"]}:{item}={gain}' for item, gain in gains.items()]
    # imu.mock's default gyro `rate` is 0.1 rad/s (a turning IMU); at rest it is 0.
    sets.append(f'114:{imu}:1=0')
    mock_cli(args.cli, endpoint, 'set', *sets)
    rate = max(1000, int(args.state_rate))
    mock_cli(args.cli, endpoint, 'set', f'5:{control["id"]}:1={rate}',
             f'5:{sensor["id"]}:1={max(int(sensor["rate_hz"]), int(args.state_rate))}')
    mock_cli(args.cli, endpoint, 'save')
    mock_reboot(args.cli, endpoint)
    instances = mock_cli(args.cli, endpoint, 'instances')
    bad = [i for i in instances if i['type'] != 5 and i['state'] != 0]
    if bad:
        raise Fail(f'mock layout instances not running: {bad}')
    agent = Proc('agent', [args.agent_bin, '--port', '0', '--token', token, '--device', f'dev={endpoint}',
                           '--retry-ms', '300', '--hello-timeout-ms', '500'])
    url = agent.next_line(15, 'READY ')[len('READY '):]
    return url, 'dev'


# ------------------------------------------------------------- ROS side

def xml_escape(text):
    return (text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;'))


def make_urdf(args, token):
    p = [('agent', args.agent), ('device', args.device)]
    if token is not None:
        p.append(('token', token))
    else:
        p.append(('token_file', args.token_file))
    p.append(('state_rate_hz', f'{args.state_rate:g}'))
    out = ['<?xml version="1.0"?>', '<robot name="drivelink_state_check">', '  <link name="base_link"/>']
    # ros2_control wants every joint of the block to exist in the URDF.
    for name, _ in args.joint:
        out += [f'  <link name="{name}_link"/>', f'  <joint name="{name}" type="continuous">',
                f'    <parent link="base_link"/><child link="{name}_link"/><axis xyz="0 1 0"/>', '  </joint>']
    out += [f'  <ros2_control name="{HW_NAME}" type="system">', '    <hardware>',
            f'      <plugin>{PLUGIN}</plugin>']
    out += [f'      <param name="{k}">{xml_escape(v)}</param>' for k, v in p]
    out.append('    </hardware>')
    for name, label in args.joint:
        out += [f'    <joint name="{name}">', f'      <param name="instance">{xml_escape(label)}</param>',
                '      <state_interface name="position"/>', '      <state_interface name="velocity"/>',
                '    </joint>']
    if args.imu:
        name, label = args.imu
        out += [f'    <sensor name="{name}">', f'      <param name="instance">{xml_escape(label)}</param>']
        out += [f'      <state_interface name="{i}"/>' for i in IMU_INTERFACES]
        out.append('    </sensor>')
    out += ['  </ros2_control>', '</robot>', '']
    return '\n'.join(out)


def make_yaml(args):
    out = ['controller_manager:', '  ros__parameters:', f'    update_rate: {int(round(args.state_rate))}',
           '    joint_state_broadcaster:', '      type: joint_state_broadcaster/JointStateBroadcaster']
    if args.imu:
        out += ['    imu_sensor_broadcaster:', '      type: imu_sensor_broadcaster/IMUSensorBroadcaster']
        out += ['imu_sensor_broadcaster:', '  ros__parameters:', f'    sensor_name: {args.imu[0]}',
                f'    frame_id: {args.imu[0]}_link']
    return '\n'.join(out) + '\n'


class Probe:
    """rclpy node: publishes /robot_description, collects /joint_states and the IMU topic."""

    def __init__(self, urdf, want_imu):
        import rclpy
        from rclpy.signals import SignalHandlerOptions
        from rclpy.executors import SingleThreadedExecutor
        from rclpy.qos import QoSProfile, DurabilityPolicy, ReliabilityPolicy, HistoryPolicy
        from std_msgs.msg import String
        from sensor_msgs.msg import JointState, Imu
        self.rclpy = rclpy
        # rclpy's own SIGINT handler would shut the context down under the checks; Ctrl-C is ours.
        rclpy.init(signal_handler_options=SignalHandlerOptions.NO)
        self.node = rclpy.create_node('drivelink_state_check')
        latched = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL,
                             reliability=ReliabilityPolicy.RELIABLE, history=HistoryPolicy.KEEP_LAST)
        self.pub = self.node.create_publisher(String, '/robot_description', latched)
        msg = String()
        msg.data = urdf
        self.pub.publish(msg)
        self.lock = threading.Lock()
        self.joints, self.imus = [], []   # (monotonic receive time, message data)
        self.node.create_subscription(JointState, '/joint_states', self._joint, 50)
        if want_imu:
            self.node.create_subscription(Imu, IMU_TOPIC, self._imu, 50)
        self.executor = SingleThreadedExecutor()
        self.executor.add_node(self.node)
        self.stop = False
        self.thread = threading.Thread(target=self._spin, daemon=True)
        self.thread.start()

    def _spin(self):
        while not self.stop:
            self.executor.spin_once(timeout_sec=0.05)

    def _joint(self, m):
        with self.lock:
            self.joints.append((time.monotonic(), {'name': list(m.name), 'position': list(m.position),
                                                   'velocity': list(m.velocity)}))

    def _imu(self, m):
        with self.lock:
            self.imus.append((time.monotonic(), {
                'q': [m.orientation.x, m.orientation.y, m.orientation.z, m.orientation.w],
                'w': [m.angular_velocity.x, m.angular_velocity.y, m.angular_velocity.z],
                'a': [m.linear_acceleration.x, m.linear_acceleration.y, m.linear_acceleration.z]}))

    def window(self, seconds):
        """Messages received during the next `seconds`."""
        with self.lock:
            j0, i0 = len(self.joints), len(self.imus)
        t0 = time.monotonic()
        time.sleep(seconds)
        t1 = time.monotonic()
        with self.lock:
            return [m for _, m in self.joints[j0:]], [m for _, m in self.imus[i0:]], t1 - t0

    def latest_joint(self):
        with self.lock:
            return self.joints[-1][1] if self.joints else None

    def hardware_components(self, timeout=10):
        """controller_manager's list_hardware_components service."""
        from controller_manager_msgs.srv import ListHardwareComponents
        client = self.node.create_client(ListHardwareComponents, '/controller_manager/list_hardware_components')
        if not client.wait_for_service(timeout_sec=timeout):
            raise Fail('/controller_manager/list_hardware_components is not available')
        future = client.call_async(ListHardwareComponents.Request())
        deadline = time.monotonic() + timeout
        while not future.done():
            if time.monotonic() > deadline:
                raise Fail('list_hardware_components did not answer')
            time.sleep(0.02)
        return future.result().component

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        try:
            self.executor.shutdown()
            self.node.destroy_node()
            self.rclpy.shutdown()
        except Exception:
            pass


def pitch_deg(q):
    """Pitch about +Y of a ZYX (yaw-pitch-roll) quaternion [x, y, z, w], degrees."""
    x, y, z, w = q
    s = max(-1.0, min(1.0, 2 * (w * y - z * x)))
    return math.degrees(math.asin(s))


def mean(xs):
    return sum(xs) / len(xs) if xs else float('nan')


# ------------------------------------------------------------------- checks

def check_streaming(args, probe):
    joints, imus, dt = probe.window(3.0)
    want = args.state_rate * 0.8
    rate = len(joints) / dt
    names = {n for m in joints for n in m['name']}
    missing = [n for n, _ in args.joint if n not in names]
    bad = sum(1 for m in joints for v in m['position'] + m['velocity'] if not finite(v))
    ok = report('1 streaming /joint_states', 'PASS' if rate >= want and not missing and not bad else 'FAIL',
                f'{rate:.1f} Hz (need >= {want:.1f}), {len(joints)} msgs in {dt:.1f} s, missing joints {missing}, '
                f'non-finite values {bad}', rate_hz=rate, messages=len(joints), missing=missing, non_finite=bad)
    if args.imu:
        irate = len(imus) / dt
        bad = sum(1 for m in imus for v in m['q'] + m['w'] + m['a'] if not finite(v))
        norms = [math.sqrt(sum(c * c for c in m['q'])) for m in imus]
        worst = max((abs(n - 1) for n in norms), default=float('nan'))
        good = irate >= want and not bad and worst <= 0.02
        ok &= report('1 streaming IMU', 'PASS' if good else 'FAIL',
                     f'{irate:.1f} Hz (need >= {want:.1f}), {len(imus)} msgs, non-finite values {bad}, '
                     f'worst | |q|-1 | {worst:.4f} (need <= 0.02)', rate_hz=irate, messages=len(imus),
                     non_finite=bad, quaternion_norm_error=worst)
    return ok


def check_imu_rest(args, probe):
    if not args.mock:
        input('Keep the robot still on its stands, then press Enter... ')
    _, imus, dt = probe.window(2.0)
    if not imus:
        return report('2 IMU at rest', 'FAIL', 'no IMU messages')
    amag = mean([math.sqrt(sum(c * c for c in m['a'])) for m in imus])
    wmax = max(math.sqrt(sum(c * c for c in m['w'])) for m in imus)
    accel_ok = abs(amag - GRAVITY) <= 0.5
    gyro_ok = wmax < 0.05
    text = (f'|a| {amag:.3f} m/s^2 (need {GRAVITY} +- 0.5), max |w| {wmax:.4f} rad/s (need < 0.05), '
            f'{len(imus)} msgs')
    vals = {'accel_magnitude': amag, 'gyro_max': wmax, 'messages': len(imus)}
    if args.mock and not accel_ok:
        # imu.mock only produces gravity when its model does; report, do not fail
        return report('2 IMU at rest', 'SKIPPED', '(mock) imu.mock does not give gravity: ' + text, **vals)
    return report('2 IMU at rest', 'PASS' if accel_ok and gyro_ok else 'FAIL', text, **vals)


def check_wheels(args, probe):
    ok = True
    for name, _ in args.joint:
        idx = None
        base = probe.latest_joint()
        if base is None or name not in base['name']:
            ok &= report(f'3 wheel {name}', 'FAIL', 'no /joint_states sample for it')
            continue
        idx = base['name'].index(name)
        input(f'Turn {name} one full revolution FORWARD (the robot\'s driving direction) by hand, '
              f'then press Enter... ')
        end = probe.latest_joint()
        moved = end['position'][idx] - base['position'][idx]
        revs = moved / (2 * math.pi)
        within = abs(moved - 2 * math.pi) <= 0.15 * 2 * math.pi
        text = f'moved {moved:+.3f} rad = {revs:+.3f} revolutions (need +2*pi = +6.283 rad +- 15 %)'
        if moved < 0 and abs(abs(moved) - 2 * math.pi) <= 0.15 * 2 * math.pi:
            text += '; NEGATIVE: the polarity is inverted (forward motion reads as backward)'
        ok &= report(f'3 wheel {name} revolution', 'PASS' if within else 'FAIL', text, position_change_rad=moved,
                     revolutions=revs)
        input(f'Hold {name} still, then press Enter... ')
        joints, _, _ = probe.window(1.0)
        vmax = max((abs(m['velocity'][m['name'].index(name)]) for m in joints if name in m['name']),
                   default=float('nan'))
        ok &= report(f'3 wheel {name} still', 'PASS' if vmax < 0.05 else 'FAIL',
                     f'max |velocity| {vmax:.4f} rad/s (need < 0.05) over {len(joints)} msgs', velocity_max=vmax)
    return ok


def check_tilt(args, probe):
    def pitch():
        _, imus, _ = probe.window(0.5)
        return mean([pitch_deg(m['q']) for m in imus if all(finite(c) for c in m['q'])])
    input('Keep the robot level and still, then press Enter (reads the starting pitch)... ')
    before = pitch()
    input('Tilt the robot nose up by about 30 degrees and hold it, then press Enter... ')
    after = pitch()
    delta = after - before
    sign = 'negative' if delta < 0 else 'positive'
    magnitude_ok = 20 <= abs(delta) <= 40
    sign_ok = delta < 0
    text = (f'pitch {before:+.1f} -> {after:+.1f} deg, change {delta:+.1f} deg (need 20..40 in magnitude); '
            f'sign {sign}, expected negative (REP 103: nose up is negative pitch about +Y, x forward, z up)')
    if magnitude_ok and not sign_ok:
        text += '; the IMU axes or mounting do not follow REP 103'
    return report('4 IMU tilt', 'PASS' if magnitude_ok and sign_ok else 'FAIL', text, pitch_before_deg=before,
                  pitch_after_deg=after, change_deg=delta, sign=sign)


def check_lease(args, probe):
    try:
        components = probe.hardware_components()
    except Fail as e:
        return report('5 component active', 'FAIL', str(e))
    found = [c for c in components if c.name == HW_NAME]
    if not found:
        return report('5 component active', 'FAIL', f'no hardware component "{HW_NAME}" (have {[c.name for c in components]})')
    c = found[0]
    state = c.state.label
    states = [i.name for i in c.state_interfaces]
    want = 2 * len(args.joint) + (len(IMU_INTERFACES) if args.imu else 0)
    ok = state == 'active' and len(states) == want
    return report('5 component active', 'PASS' if ok else 'FAIL',
                  f'component "{c.name}" ({c.plugin_name}) is {state} (need active), {len(states)} state interfaces '
                  f'(need {want}); active means the agent lease is held and the groups stream',
                  state=state, plugin=c.plugin_name, state_interfaces=states)


# --------------------------------------------------------------------- main

def have_ros(distro_hint=''):
    if not shutil.which('ros2'):
        raise Fail('ros2 is not on PATH: source ROS first: source /opt/ros/$ROS_DISTRO/setup.bash')
    run = subprocess.run(['ros2', 'pkg', 'prefix', 'drivelink_hardware'], capture_output=True, text=True, timeout=60)
    if run.returncode != 0:
        raise Fail('the package drivelink_hardware is not found: source the workspace install space first '
                   '(source INSTALL_DIR/setup.bash after colcon build)')
    return run.stdout.strip()


def wait_active(probe, node, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            for c in probe.hardware_components(timeout=2):
                if c.name == HW_NAME and c.state.label == 'active':
                    return
        except Fail:
            pass
        if not node.alive():
            break
        time.sleep(0.5)
    what = 'ros2_control_node exited' if not node.alive() else f'did not become active within {timeout} s'
    raise Fail(f'the hardware component "{HW_NAME}": {what}; '
               f'ros2_control_node output:\n{node.tail(40)}')


def run_spawner(name, yaml_path, node):
    argv = ['ros2', 'run', 'controller_manager', 'spawner', name, '-c', '/controller_manager',
            '--controller-manager-timeout', '40', '--param-file', str(yaml_path)]
    spawner = Proc(f'spawner-{name}', argv)
    deadline = time.monotonic() + 60
    while spawner.alive() and time.monotonic() < deadline:
        if not node.alive():
            break
        time.sleep(0.2)
    if spawner.alive():
        spawner.stop()
        raise Fail(f'spawner {name} did not finish within 60 s:\n' + spawner.tail())
    if spawner.p.returncode != 0:
        raise Fail(f'spawner {name} failed ({spawner.p.returncode}):\n{spawner.tail()}\n'
                   f'ros2_control_node:\n{node.tail(40)}')


def parse_pair(text):
    if '=' not in text or text.startswith('=') or text.endswith('='):
        raise argparse.ArgumentTypeError(f'expected NAME=LABEL, got {text!r}')
    name, label = text.split('=', 1)
    return name, label


def run(args, tmp):
    token = None
    if args.mock:
        token = secrets.token_hex(12)
        os.environ.setdefault('ROS_DOMAIN_ID', str(secrets.randbelow(100) + 1))
        os.environ.setdefault('ROS_AUTOMATIC_DISCOVERY_RANGE', 'LOCALHOST')
        args.agent, args.device = start_mock(args, tmp, token)
        args.joint = [('left_wheel', 'left'), ('right_wheel', 'right')]
        args.imu = ('imu', 'imu')
    elif args.token:
        token = args.token
    if not args.joint:
        raise Fail('give at least one --joint NAME=LABEL (or --mock)')
    if not args.device:
        raise Fail('--device ALIAS_OR_ID is required (or --mock)')
    if not 0 < args.state_rate <= 10000:
        raise Fail('--state-rate must be in (0, 10000]')

    urdf, yaml = tmp / 'drivelink.urdf', tmp / 'controllers.yaml'
    urdf.write_text(make_urdf(args, token))
    yaml.write_text(make_yaml(args))
    probe = Probe(urdf.read_text(), bool(args.imu))
    try:
        node = Proc('ros2_control_node', ['ros2', 'run', 'controller_manager', 'ros2_control_node',
                                          '--ros-args', '--params-file', str(yaml)])
        wait_active(probe, node)
        run_spawner('joint_state_broadcaster', yaml, node)
        if args.imu:
            run_spawner('imu_sensor_broadcaster', yaml, node)
        time.sleep(0.5)
        ok = check_streaming(args, probe)
        if args.imu:
            ok &= check_imu_rest(args, probe)
        else:
            report('2 IMU at rest', 'SKIPPED', 'no --imu given')
        if args.mock:
            report('3 wheel revolution', 'SKIPPED', '(mock) needs hands on real wheels')
            report('4 IMU tilt', 'SKIPPED', '(mock) needs a real tilt')
        else:
            ok &= check_wheels(args, probe)
            if args.imu:
                ok &= check_tilt(args, probe)
            else:
                report('4 IMU tilt', 'SKIPPED', 'no --imu given')
        ok &= check_lease(args, probe)
        if not node.alive():
            ok &= report('ros2_control_node', 'FAIL', 'exited during the checks:\n' + node.tail(30))
        return ok
    finally:
        probe.close()


def main():
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--agent', default='tcp://127.0.0.1:47620')
    ap.add_argument('--device', help='device alias or 12-hex id behind the agent')
    ap.add_argument('--token')
    ap.add_argument('--token-file', default='/etc/drivelink/token')
    ap.add_argument('--joint', type=parse_pair, action='append', default=[], metavar='NAME=LABEL')
    ap.add_argument('--imu', type=parse_pair, metavar='NAME=LABEL')
    ap.add_argument('--state-rate', type=float, default=100)
    ap.add_argument('--ws', help='workspace install space; informational: source it before running')
    ap.add_argument('--report', help='write every measured value and verdict here as JSON')
    ap.add_argument('--mock', action='store_true', help='run against drivelink-vdev behind drivelink-agent')
    ap.add_argument('--vdev', default=env('DRIVELINK_VDEV'))
    ap.add_argument('--agent-bin', dest='agent_bin', default=env('DRIVELINK_AGENT'))
    ap.add_argument('--cli', default=env('DRIVELINK_CLI'))
    args = ap.parse_args()

    started = time.monotonic()
    ok = False
    def interrupt(*_):
        raise KeyboardInterrupt()
    # explicit, so Ctrl-C and SIGTERM also work when started in the background (SIGINT ignored)
    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    with tempfile.TemporaryDirectory(prefix='drivelink-ros-check-') as tmp:
        try:
            print(f'plugin package: {have_ros()}', flush=True)
            ok = run(args, Path(tmp))
        except Fail as failure:
            print(f'FAIL {failure}', file=sys.stderr, flush=True)
            RESULTS.append({'check': 'setup', 'verdict': 'FAIL', 'detail': str(failure), 'values': {}})
        except KeyboardInterrupt:
            print('interrupted', file=sys.stderr, flush=True)
            RESULTS.append({'check': 'run', 'verdict': 'FAIL', 'detail': 'interrupted', 'values': {}})
        finally:
            stop_all()
    verdict = 'PASS' if ok else 'FAIL'
    print(f'{verdict} ros state check ({len(RESULTS)} results, {time.monotonic() - started:.0f} s)', flush=True)
    if args.report:
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps({
            'verdict': verdict, 'mock': args.mock, 'agent': args.agent, 'device': args.device,
            'state_rate_hz': args.state_rate, 'joints': args.joint, 'imu': args.imu,
            'checks': RESULTS}, indent=2) + '\n')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
