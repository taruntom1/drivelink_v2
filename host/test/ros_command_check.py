#!/usr/bin/env python3
"""H4b deferred hardware check: DriveLink COMMANDS through the real ros2_control stack.

THIS CHECK MOVES THE MOTORS. The robot must be on stands, wheels OFF the ground.

Runs the drivelink_hardware plugin (drivelink_hardware/DriveLinkSystem) inside
`ros2_control_node` against a real ESP behind drivelink-agent on the Pi, with
joint_state_broadcaster and, per wheel, forward_command_controller/
ForwardCommandController instances for the `velocity`, `position` and `duty`
command interfaces (all loaded inactive; the check switches them with the
controller_manager's switch_controller service so one wheel runs one mode at a
time), plus one more velocity controller that is never published to. Commands
go out on /<controller>/commands (std_msgs/Float64MultiArray); the measured
state is /joint_states. The device's arm state and the controller's `mode`
param are read through drivelink-agent with drivelink-cli as an observer
(`status`, `get`; no lease needed for reads).

First-run procedure, every time (it is the one of host/test/motor_smoke.py):
wheels off the ground, a current-limited supply, a low duty cap, and a hand on
the power switch. The script prints it and asks you to type `yes`;
--confirm-first-run skips the prompt (it still prints it). Besides that it
refuses to start unless every joint's controller `duty_limit` is at most
--duty-cap (set it with the CLI, or pass --set-duty-limit to write it live,
not saved, for this run and restore it at the end). The duty check commands at
most --duty-cap, whatever --duty says.

Checks, in order (each printed as PASS/FAIL with the numbers). Check 1 failing
stops the run before anything is commanded:
  1 active     the "drivelink" hardware component is active with every command
               interface exported and none claimed; the device is disarmed
  2 velocity   per wheel: activate its velocity controller; the device is armed
               and `mode` reads velocity. Command +V (--velocity, default 2 rad/s)
               for 2 s: the measured velocity over the last second has the same
               sign (polarity) and is within --velocity-tolerance (default 0.3 =
               30 %) of V. Command 0: |velocity| < 0.2 rad/s within 2 s.
               Deactivate: the device is disarmed, the component still active
  3 position   per wheel: activate its position controller; `mode` reads
               position. Command position + --position-step (default 1 rad): it is
               within --position-tolerance (default 0.15 rad) after 3 s. Deactivate
  4 duty       per wheel: activate its duty controller; `mode` reads open_loop.
               Command +D (--duty, default 0.15, at most --duty-cap, default
               0.2) for 1.5 s: the velocity is positive (> 0.05 rad/s). Command 0:
               the wheel stops. Deactivate
  5 NaN hold   the first wheel's never-published velocity controller: the device
               stays armed and the wheel is still (|velocity| < 0.1 rad/s) for 1 s
               (the plugin sends a hold for the NaN command); deactivate
  5b mode      the first wheel's velocity controller at min(--velocity, 1) rad/s
     switch    for 1 s, then one switch hands over to its position controller
               while armed: the device stays armed (no DISARM), `mode` reads
               position, with no command the wheel stays within -0.3/+1.0 rad
               of where it was at the switch (braking, never a jump back) for
               1.5 s, then the position controller answers a +0.5 rad step
               (at least +0.25, within 0.3 rad of the target after 3 s;
               accuracy is check 3's, from rest)
  6 GUI Take   with a velocity controller active and command 0, a second agent
               client (drivelink-cli --lease, named gui-take-sim) takes the lease,
               as the GUI would. Within 1 s the hardware component is no longer
               active and the device is disarmed. The CLI's lease is released
               afterwards (the CLI is stopped) and the device stays disarmed;
               nothing re-arms by itself

Every exit path (a failed check, Ctrl-C, SIGTERM) deactivates the controllers,
stops ros2_control_node, restores any duty_limit this run set, and makes sure
the device ends disarmed: after the node is stopped the script sends DISARM
through the CLI (any client may) and reads the arm state back; a device that is
still armed then is a FAIL printed loudly, and then you cut the power.

Run it in a shell where ROS 2 and the workspace install space are sourced
(`source /opt/ros/$ROS_DISTRO/setup.bash; source INSTALL/setup.bash`); it needs
`ros2`, the package drivelink_hardware (`ros2 pkg prefix drivelink_hardware`)
and drivelink-cli (--cli, DRIVELINK_CLI or PATH). Stop anything else that
drives the device: the plugin refuses a lease held by another client (the GUI).

Real hardware (the robot on stands, wheels free, ESP behind the agent):

  python3 host/test/ros_command_check.py --device <alias> --token-file /etc/drivelink/token \\
      --joint left_wheel=<left label> --joint right_wheel=<right label> \\
      --confirm-first-run --report ros-command-check.json

Joint names (--joint NAME=LABEL) are the URDF joint names; the labels are the
instance labels of the device layout (controller.cascade_pid instances). Add
--agent tcp://HOST:PORT if the agent is not on 127.0.0.1:47620.

Pass means exit 0 and a PASS line for every check. A negative velocity in
check 2 means the polarity is inverted (fix the layout or the URDF, not this
script). Real motor behaviour is unverified until this has passed on the robot.

--mock needs no hardware and no prompts: it starts drivelink-vdev behind
drivelink-agent with the example layout (controllers "left" and "right",
controller.cascade_pid, on actuator.mock and feedback.mock; the wheels are
left_wheel and right_wheel) with velocity and position gains that track the
mock plant, and runs every check:

  python3 host/test/ros_command_check.py --mock --vdev V --agent-bin A --cli C
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
# The sibling H4a script owns the shared plumbing (child processes with
# process-group teardown, PASS/FAIL printing, the vdev CLI helper, the
# "component active" wait); this script adds the command path.
from ros_state_check import (RESULTS, Fail, HW_NAME, PLUGIN, Proc, have_ros,  # noqa: E402
                             mean, mock_cli, mock_reboot, parse_pair, report, stop_all, xml_escape)

COMMAND_KINDS = ('velocity', 'position', 'duty')   # URDF order
MODE_OF = {'velocity': 'velocity', 'position': 'position', 'duty': 'open_loop'}
CLI_CLIENT = 'gui-take-sim'
CTRL_TYPE = 'forward_command_controller/ForwardCommandController'
NAN_HOLD = 'nan_hold'

# controller.cascade_pid (type 0x20) item ids and the mock layout's tuning.
PID_TYPE = 32
ITEM_DUTY_LIMIT, ITEM_VEL_KP, ITEM_VEL_KI, ITEM_POS_KP, ITEM_POS_KI = 17, 32, 33, 48, 49
# Chosen against actuator.mock (gain 10 rad/s per unit duty, tau 0.1 s, deadband
# 0.02): velocity PI kp 0.12, ki 0.8 (closed loop poles about -6 and -12, 2 rad/s
# settles within 0.5 s, the 0.2 steady duty is ten times the deadband); the
# position loop is P 3 (a 1 rad step starts at 3 rad/s, time constant about
# 0.3 s) with a small I 0.5 that pushes through the deadband. These are the
# gains host/test/motor_smoke.py's mock run tracks with.
MOCK_GAINS = {ITEM_VEL_KP: 0.12, ITEM_VEL_KI: 0.8, ITEM_POS_KP: 3.0, ITEM_POS_KI: 0.5}


class Context:
    """What the checks and the shutdown share."""

    def __init__(self, args):
        self.args = args
        self.probe = None
        self.node = None
        self.obs = None
        self.refs = {}            # joint name -> JointRefs
        self.restore_duty = []    # (ref, original typed value)
        self.lease_proc = None


# ------------------------------------------------------------- mock topology

def start_mock(args, tmp, token):
    """vdev with the example layout and tracking gains behind drivelink-agent. Returns (agent url, alias)."""
    for name in ('vdev', 'agent_bin', 'cli'):
        path = getattr(args, name)
        if not path or not os.path.isfile(path):
            raise Fail(f'--mock needs --{name.replace("_", "-")} (or DRIVELINK_{name.split("_")[0].upper()}): {path!r}')
    vdev = Proc('vdev', [args.vdev, '--port', '0', '--state', str(tmp / 'vdev')])
    endpoint = vdev.next_line(15, 'READY ')[len('READY '):]
    info = mock_cli(args.cli, endpoint, 'info')
    control = next(loop for loop in info['loops'] if loop['realtime'])
    # Per side an actuator.mock (112), a feedback.mock (113) and a
    # controller.cascade_pid (32) on the control loop. (No IMU: unused here.)
    act, fb, pid = 112, 113, PID_TYPE
    made = {}
    for side in ('left', 'right'):
        made[side] = {kind: mock_cli(args.cli, endpoint, 'add', t, label, control['id'])['instance']
                      for kind, t, label in (('act', act, f'{side}_act'), ('fb', fb, f'{side}_enc'))}
        made[side]['pid'] = mock_cli(args.cli, endpoint, 'add', pid, side, control['id'])['instance']
    sets = []
    for side, m in made.items():
        sets += [f'{fb}:{m["fb"]}:1={(act << 8) | m["act"]}',
                 f'{pid}:{m["pid"]}:1={(act << 8) | m["act"]}', f'{pid}:{m["pid"]}:2={(fb << 8) | m["fb"]}']
        sets += [f'{pid}:{m["pid"]}:{item}={gain}' for item, gain in MOCK_GAINS.items()]
    mock_cli(args.cli, endpoint, 'set', *sets)
    mock_cli(args.cli, endpoint, 'set', f'5:{control["id"]}:1={max(1000, int(args.state_rate))}')
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


# ----------------------------------------------------- the CLI as an observer

class Observer:
    """drivelink-cli through the agent: arm state, the mode param, DISARM, the lease."""

    def __init__(self, cli, agent, device, token):
        self.cli, self.agent, self.device = cli, agent, device
        self.env = dict(os.environ, DRIVELINK_AGENT_TOKEN=token)

    def argv(self, *command, device=True, lease=False):
        argv = [self.cli, '--endpoint', self.agent]
        if device:
            argv += ['--device', self.device]
        if lease:
            argv += ['--client-name', CLI_CLIENT, '--lease']
        return argv + ['--json', *map(str, command)]

    def call(self, *command, device=True, code=0, timeout=15):
        try:
            run = subprocess.run(self.argv(*command, device=device), capture_output=True, text=True,
                                 timeout=timeout, env=self.env)
        except subprocess.TimeoutExpired:
            raise Fail(f'drivelink-cli {" ".join(map(str, command))}: no answer within {timeout} s')
        try:
            envelope = json.loads(run.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            raise Fail(f'drivelink-cli {" ".join(map(str, command))}: no JSON on stdout (exit {run.returncode}): '
                       f'{run.stderr.strip()}')
        if run.returncode != code or envelope.get('ok') != (code == 0):
            raise Fail(f'drivelink-cli {" ".join(map(str, command))}: exit {run.returncode}: '
                       f'{run.stdout.strip()} {run.stderr.strip()}')
        return envelope.get('data', envelope)

    def wait_online(self, timeout=20):
        """The agent refuses clients of a device it has not opened yet: retry `info` until it answers."""
        deadline = time.monotonic() + timeout
        while True:
            try:
                return self.call('info')
            except Fail as e:
                if time.monotonic() > deadline:
                    raise Fail(f'the device did not come online through the agent within {timeout} s: {e}')
                time.sleep(0.3)

    def arm_state(self):
        """0 disarmed, 1 armed, 2 fault (the next status frame of the device, at 10 Hz)."""
        return self.call('status')['arm_state']

    def wait_arm(self, want, timeout):
        """Poll the arm state until it is `want`. Returns (reached, seconds, last state)."""
        start = time.monotonic()
        state = None
        while True:
            state = self.arm_state()
            if state == want:
                return True, time.monotonic() - start, state
            if time.monotonic() - start >= timeout:
                return False, time.monotonic() - start, state
            time.sleep(0.05)

    def resolve(self, name, label):
        """The refs of a joint's controller instance: mode, duty_limit, and the mode enum."""
        instances = self.call('instances')
        found = [i for i in instances if i.get('label') == label]
        if len(found) != 1:
            raise Fail(f'joint {name}: {len(found)} instances labelled {label!r} (labels: '
                       f'{sorted(i.get("label") for i in instances)})')
        inst = found[0]
        items = {d['name']: d for d in self.call('items', inst['type'])}
        for need in ('mode', 'duty_limit'):
            if need not in items or items[need]['kind'] != 0:
                raise Fail(f'joint {name}: instance {label!r} (type {inst["type"]}) has no param {need!r}: '
                           f'this check needs controller.cascade_pid')
        base = f'{inst["type"]}:{inst["instance"]}'
        return JointRefs(name, label, f'{base}:{items["mode"]["id"]}', f'{base}:{items["duty_limit"]["id"]}',
                         self.enum_names(inst['type'], items['mode']['id']))

    def enum_names(self, type_, item):
        """{value: name} of an enum item (the `enums TYPE ITEM` listing), {} when it cannot be read."""
        try:
            data = self.call('enums', type_, item)
        except Fail:
            return {}
        rows = data if isinstance(data, list) else data.get('enums', data.get('values', []))
        names = {}
        for row in rows if isinstance(rows, list) else []:
            if isinstance(row, dict) and 'name' in row and 'value' in row:
                names[row['value']] = row['name']
        return names

    def mode(self, refs):
        """The mode param's name ('velocity', ...), read live."""
        raw = self.call('get', refs.mode)['typed_values'][0]
        if isinstance(raw, str):
            return raw
        return refs.mode_names.get(raw, {0: 'open_loop', 1: 'velocity', 2: 'position'}.get(raw, str(raw)))

    def duty_limit(self, refs):
        return float(self.call('get', refs.duty_limit)['typed_values'][0])

    def holder(self):
        """The agent's record of this device: (holder_id, holder_name, holder_kind) or None."""
        data = self.call('agent-devices', device=False)
        for d in data['devices']:
            if self.device in (d['alias'], d['device_id']):
                return d['holder_id'], d['holder_name'], d['holder_kind']
        return None

    def ensure_disarmed(self):
        """Last resort: DISARM (any client may) until the status says disarmed. Returns (ok, text)."""
        last = 'no reading'
        for attempt in range(3):
            try:
                self.call('disarm', timeout=10)
                reached, took, state = self.wait_arm(0, 3.0)
            except Fail as e:
                last = str(e)
                time.sleep(0.5)
                continue
            if reached:
                return True, f'DISARM sent; arm state 0 after {took:.2f} s'
            last = f'arm state {state} {took:.1f} s after DISARM'
        return False, f'could not verify the device is disarmed ({last})'


class JointRefs:
    def __init__(self, name, label, mode, duty_limit, mode_names):
        self.name, self.label, self.mode, self.duty_limit, self.mode_names = name, label, mode, duty_limit, mode_names


# ------------------------------------------------------------- ROS side

def make_urdf(args, token):
    p = [('agent', args.agent), ('device', args.device)]
    if token is not None:
        p.append(('token', token))
    else:
        p.append(('token_file', args.token_file))
    p.append(('state_rate_hz', f'{args.state_rate:g}'))
    out = ['<?xml version="1.0"?>', '<robot name="drivelink_command_check">', '  <link name="base_link"/>']
    for name, _ in args.joint:
        out += [f'  <link name="{name}_link"/>', f'  <joint name="{name}" type="continuous">',
                f'    <parent link="base_link"/><child link="{name}_link"/><axis xyz="0 1 0"/>', '  </joint>']
    out += [f'  <ros2_control name="{HW_NAME}" type="system">', '    <hardware>',
            f'      <plugin>{PLUGIN}</plugin>']
    out += [f'      <param name="{k}">{xml_escape(v)}</param>' for k, v in p]
    out.append('    </hardware>')
    for name, label in args.joint:
        out += [f'    <joint name="{name}">', f'      <param name="instance">{xml_escape(label)}</param>']
        out += [f'      <command_interface name="{k}"/>' for k in COMMAND_KINDS]
        out += ['      <state_interface name="position"/>', '      <state_interface name="velocity"/>', '    </joint>']
    out += ['  </ros2_control>', '</robot>', '']
    return '\n'.join(out)


def controller_name(joint, kind):
    return f'{joint}_{kind}'


def controller_table(args):
    """[(controller name, joint, interface)]: three per wheel, then the never-published one."""
    table = [(controller_name(j, k), j, k) for j, _ in args.joint for k in COMMAND_KINDS]
    table.append((NAN_HOLD, args.joint[0][0], 'velocity'))
    return table


def make_yaml(args):
    table = controller_table(args)
    out = ['controller_manager:', '  ros__parameters:', f'    update_rate: {int(round(args.state_rate))}',
           '    joint_state_broadcaster:', '      type: joint_state_broadcaster/JointStateBroadcaster']
    out += [f'    {name}:\n      type: {CTRL_TYPE}' for name, _, _ in table]
    for name, joint, kind in table:
        out += [f'{name}:', '  ros__parameters:', f'    joints: [{joint}]', f'    interface_name: {kind}']
    return '\n'.join(out) + '\n'


class Probe:
    """rclpy node: /robot_description, /joint_states, the command topics, the controller_manager services."""

    def __init__(self, urdf, controllers):
        import rclpy
        from rclpy.signals import SignalHandlerOptions
        from rclpy.executors import SingleThreadedExecutor
        from rclpy.qos import QoSProfile, DurabilityPolicy, ReliabilityPolicy, HistoryPolicy
        from std_msgs.msg import String, Float64MultiArray
        from sensor_msgs.msg import JointState
        self.rclpy, self.Float64MultiArray = rclpy, Float64MultiArray
        # rclpy's own SIGINT handler would shut the context down under the checks; Ctrl-C is ours.
        rclpy.init(signal_handler_options=SignalHandlerOptions.NO)
        self.node = rclpy.create_node('drivelink_command_check')
        latched = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL,
                             reliability=ReliabilityPolicy.RELIABLE, history=HistoryPolicy.KEEP_LAST)
        if urdf is not None:
            self.pub = self.node.create_publisher(String, '/robot_description', latched)
            msg = String()
            msg.data = urdf
            self.pub.publish(msg)
        self.command_pubs = {c: self.node.create_publisher(Float64MultiArray, f'/{c}/commands', 10)
                             for c in controllers}
        self.lock = threading.Lock()
        self.joints = []   # (monotonic receive time, {'name', 'position', 'velocity'})
        self.node.create_subscription(JointState, '/joint_states', self._joint, 100)
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

    def mark(self):
        with self.lock:
            return len(self.joints)

    def since(self, mark):
        with self.lock:
            return list(self.joints[mark:])

    def latest_joint(self):
        with self.lock:
            return self.joints[-1][1] if self.joints else None

    def publish(self, controller, values):
        msg = self.Float64MultiArray()
        msg.data = [float(v) for v in values]
        self.command_pubs[controller].publish(msg)

    def call(self, srv, name, request, timeout):
        client = self.node.create_client(srv, name)
        try:
            if not client.wait_for_service(timeout_sec=timeout):
                raise Fail(f'{name} is not available')
            future = client.call_async(request)
            deadline = time.monotonic() + timeout
            while not future.done():
                if time.monotonic() > deadline:
                    raise Fail(f'{name} did not answer within {timeout:g} s')
                time.sleep(0.01)
            return future.result()
        finally:
            self.node.destroy_client(client)

    def hardware_components(self, timeout=10):
        from controller_manager_msgs.srv import ListHardwareComponents
        return self.call(ListHardwareComponents, '/controller_manager/list_hardware_components',
                         ListHardwareComponents.Request(), timeout).component

    def controllers(self, timeout=10):
        from controller_manager_msgs.srv import ListControllers
        return self.call(ListControllers, '/controller_manager/list_controllers',
                         ListControllers.Request(), timeout).controller

    def switch(self, activate, deactivate, timeout=15):
        """The switch_controllers service, STRICT. Returns (ok, message)."""
        from controller_manager_msgs.srv import SwitchController
        req = SwitchController.Request()
        req.activate_controllers, req.deactivate_controllers = list(activate), list(deactivate)
        req.strictness = SwitchController.Request.STRICT
        req.activate_asap = False
        req.timeout.sec, req.timeout.nanosec = 10, 0
        res = self.call(SwitchController, '/controller_manager/switch_controller', req, timeout)
        return res.ok, res.message

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        try:
            self.executor.shutdown()
            self.node.destroy_node()
            self.rclpy.shutdown()
        except Exception:
            pass


def series(samples, joint):
    """[(t, position, velocity)] of one joint from Probe samples."""
    out = []
    for t, m in samples:
        if joint in m['name']:
            i = m['name'].index(joint)
            out.append((t, m['position'][i], m['velocity'][i]))
    return out


def drive(probe, controller, values, seconds, period=0.05):
    """Publish `values` at 20 Hz for `seconds` (none when values is None). Returns the joint samples."""
    mark = probe.mark()
    end = time.monotonic() + seconds
    while True:
        if values is not None:
            probe.publish(controller, values)
        left = end - time.monotonic()
        if left <= 0:
            break
        time.sleep(min(period, left))
    return probe.since(mark)


def settle(ctx, controller, joint, values, seconds, limit):
    """Publish `values` until |velocity| < limit (held 0.2 s) or `seconds`. Returns (settled, seconds, last |v|)."""
    start = time.monotonic()
    below_since, v = None, float('nan')
    while time.monotonic() - start < seconds:
        mark = ctx.probe.mark()
        ctx.probe.publish(controller, values)
        time.sleep(0.05)
        recent = series(ctx.probe.since(mark), joint)
        if recent:
            v = abs(recent[-1][2])
            if v < limit:
                below_since = below_since or time.monotonic()
                if time.monotonic() - below_since >= 0.2:
                    return True, time.monotonic() - start, v
            else:
                below_since = None
    return False, time.monotonic() - start, v


@contextlib.contextmanager
def claimed(ctx, controller):
    """Activate one controller; always deactivate it on the way out."""
    ok, message = ctx.probe.switch([controller], [])
    state = {'ok': ok, 'message': message}
    try:
        yield state
    finally:
        if ok:
            try:
                state['released'], state['release_message'] = ctx.probe.switch([], [controller])
            except Fail as e:
                state['released'], state['release_message'] = False, str(e)


def alive(ctx):
    if not ctx.node.alive():
        raise Fail('ros2_control_node exited during the checks:\n' + ctx.node.tail(30))


def component(ctx):
    found = [c for c in ctx.probe.hardware_components() if c.name == HW_NAME]
    return found[0] if found else None


# ------------------------------------------------------------------- checks

def check_active(ctx):
    args = ctx.args
    c = component(ctx)
    if c is None:
        return report('1 active', 'FAIL', f'no hardware component "{HW_NAME}"')
    cmds = [i.name for i in c.command_interfaces]
    held = [i.name for i in c.command_interfaces if i.is_claimed]
    want = len(COMMAND_KINDS) * len(args.joint)
    reached, took, arm = ctx.obs.wait_arm(0, 2.0)
    ok = c.state.label == 'active' and len(cmds) == want and not held and reached
    return report('1 active', 'PASS' if ok else 'FAIL',
                  f'component "{c.name}" is {c.state.label} (need active), {len(cmds)} command interfaces (need {want}), '
                  f'claimed {held} (need none), device arm state {arm} (need 0 = disarmed)',
                  state=c.state.label, command_interfaces=cmds, claimed=held, arm_state=arm)


def armed_and_mode(ctx, name, refs, kind, label):
    """After activating a controller: the device is armed and `mode` reads the mode of `kind`."""
    reached, took, arm = ctx.obs.wait_arm(1, 2.0)
    mode = 'unreadable'
    deadline = time.monotonic() + 2.0
    while True:
        mode = ctx.obs.mode(refs)
        if mode == MODE_OF[kind] or time.monotonic() > deadline:
            break
        time.sleep(0.1)
    ok = reached and mode == MODE_OF[kind]
    return report(f'{label} {kind} {name} armed and mode', 'PASS' if ok else 'FAIL',
                  f'arm state {arm} after {took:.2f} s (need 1 = armed), mode "{mode}" (need "{MODE_OF[kind]}")',
                  arm_state=arm, mode=mode, arm_seconds=took)


def released(ctx, name, kind, label, state):
    """After deactivating: the device is disarmed and the component still active."""
    if not state.get('released'):
        return report(f'{label} {kind} {name} release', 'FAIL',
                      f'deactivating the controller failed: {state.get("release_message")}')
    reached, took, arm = ctx.obs.wait_arm(0, 2.0)
    c = component(ctx)
    comp_state = c.state.label if c else 'missing'
    ok = reached and comp_state == 'active'
    return report(f'{label} {kind} {name} release', 'PASS' if ok else 'FAIL',
                  f'after deactivating: arm state {arm} after {took:.2f} s (need 0 = disarmed), component {comp_state} '
                  f'(need active)', arm_state=arm, component=comp_state)


def run_mode(ctx, label, kind, body):
    """One wheel after another: activate the kind's controller, arm/mode check, `body`, deactivate, release check."""
    ok = True
    for name, _ in ctx.args.joint:
        alive(ctx)
        ctrl = controller_name(name, kind)
        with claimed(ctx, ctrl) as state:
            if not state['ok']:
                ok &= report(f'{label} {kind} {name} activate', 'FAIL',
                             f'switch_controllers refused {ctrl}: {state["message"]}\n'
                             f'ros2_control_node:\n{ctx.node.tail(15)}')
                continue
            if armed_and_mode(ctx, name, ctx.refs[name], kind, label):
                ok &= body(ctx, name, ctrl)
            else:
                ok = False
        ok &= released(ctx, name, kind, label, state)
    return ok


def body_velocity(ctx, name, ctrl):
    args = ctx.args
    drive_s = 2.0
    t_end = time.monotonic() + drive_s
    samples = drive(ctx.probe, ctrl, [args.velocity], drive_s)
    late = [v for t, _, v in series(samples, name) if t >= t_end - 1.0]
    if not late:
        return report(f'2 velocity {name} tracking', 'FAIL', 'no /joint_states samples in the last second')
    v = mean(late)
    error = abs(v - args.velocity) / args.velocity
    same_sign = v > 0
    text = (f'commanded {args.velocity:+g} rad/s for {drive_s:g} s: measured {v:+.3f} rad/s over the last 1 s '
            f'({len(late)} samples), error {100 * error:.1f} % (need <= {100 * args.velocity_tolerance:.0f} %), '
            f'sign {"matches" if same_sign else "OPPOSITE"}')
    if not same_sign and v < -0.5 * args.velocity:
        text += '; the polarity is inverted (a positive command turns the wheel backwards)'
    ok = report(f'2 velocity {name} tracking', 'PASS' if same_sign and error <= args.velocity_tolerance else 'FAIL',
                text, commanded=args.velocity, measured=v, error=error)
    stopped, took, last = settle(ctx, ctrl, name, [0.0], 2.0, 0.2)
    ok &= report(f'2 velocity {name} stop', 'PASS' if stopped else 'FAIL',
                 f'commanded 0: |velocity| {last:.3f} rad/s after {took:.2f} s (need < 0.2 within 2 s)',
                 settle_seconds=took, velocity=last)
    return ok


def body_position(ctx, name, ctrl):
    args = ctx.args
    base = ctx.probe.latest_joint()
    if base is None or name not in base['name']:
        return report(f'3 position {name} step', 'FAIL', 'no /joint_states sample for it')
    p0 = base['position'][base['name'].index(name)]
    target = p0 + args.position_step
    t0 = time.monotonic()
    samples = drive(ctx.probe, ctrl, [target], 3.0)
    s = series(samples, name)
    tail = [p for t, p, _ in s if t >= t0 + 3.0 - 0.2]
    if not tail:
        return report(f'3 position {name} step', 'FAIL', 'no /joint_states samples at the end')
    final = mean(tail)
    error = abs(final - target)
    reach = next((t - t0 for t, p, _ in s if abs(p - target) <= args.position_tolerance), None)
    text = (f'from {p0:+.3f} to {target:+.3f} rad: {final:+.3f} rad after 3 s, error {error:.3f} rad '
            f'(need <= {args.position_tolerance:g}), first within tolerance '
            f'{"after %.2f s" % reach if reach is not None else "never"}')
    return report(f'3 position {name} step', 'PASS' if error <= args.position_tolerance else 'FAIL', text,
                  start=p0, target=target, final=final, error=error, reach_seconds=reach)


def body_duty(ctx, name, ctrl):
    args = ctx.args
    duty = min(args.duty, args.duty_cap)
    t_end = time.monotonic() + 1.5
    samples = drive(ctx.probe, ctrl, [duty], 1.5)
    late = [v for t, _, v in series(samples, name) if t >= t_end - 0.5]
    peak = max((v for _, _, v in series(samples, name)), default=float('nan'))
    if not late:
        return report(f'4 duty {name} drive', 'FAIL', 'no /joint_states samples')
    v = mean(late)
    ok = report(f'4 duty {name} drive', 'PASS' if v > 0.05 else 'FAIL',
                f'duty {duty:+g} for 1.5 s: velocity {v:+.3f} rad/s over the last 0.5 s, peak {peak:+.3f} '
                f'(need > +0.05)', duty=duty, velocity=v, peak=peak)
    stopped, took, last = settle(ctx, ctrl, name, [0.0], 2.0, 0.2)
    ok &= report(f'4 duty {name} stop', 'PASS' if stopped else 'FAIL',
                 f'commanded 0: |velocity| {last:.3f} rad/s after {took:.2f} s (need < 0.2 within 2 s)',
                 settle_seconds=took, velocity=last)
    return ok


def check_nan_hold(ctx):
    name = ctx.args.joint[0][0]
    refs = ctx.refs[name]
    alive(ctx)
    with claimed(ctx, NAN_HOLD) as state:
        if not state['ok']:
            return report('5 NaN hold', 'FAIL', f'switch_controllers refused {NAN_HOLD}: {state["message"]}')
        # Nothing is published to this controller, so its command stays NaN.
        samples = drive(ctx.probe, NAN_HOLD, None, 1.0)
        reached, took, arm = ctx.obs.wait_arm(1, 1.0)
        mode = ctx.obs.mode(refs)
        vmax = max((abs(v) for _, _, v in series(samples, name)), default=float('nan'))
        ok = report('5 NaN hold', 'PASS' if reached and vmax < 0.1 and len(samples) else 'FAIL',
                    f'no command published for 1 s: arm state {arm} (need 1 = still armed), mode "{mode}", '
                    f'max |velocity| {vmax:.4f} rad/s (need < 0.1), {len(samples)} /joint_states msgs',
                    arm_state=arm, mode=mode, velocity_max=vmax)
    return released(ctx, name, 'velocity', '5', state) and ok


def check_mode_switch(ctx):
    """5b: a mode change while armed, in one controller_manager switch: the wheel's velocity controller
    hands over to its position controller. The device stays armed (no DISARM), the mode reads
    position, the wheel holds where it was instead of heading anywhere else, and the position
    controller then moves it."""
    args = ctx.args
    name = args.joint[0][0]
    refs = ctx.refs[name]
    vel, pos = controller_name(name, 'velocity'), controller_name(name, 'position')
    alive(ctx)
    ok, message = ctx.probe.switch([vel], [])
    if not ok:
        return report('5b mode switch activate', 'FAIL', f'switch_controllers refused {vel}: {message}')
    active = vel
    try:
        reached, _, arm = ctx.obs.wait_arm(1, 2.0)
        if not reached:
            return report('5b mode switch armed', 'FAIL', f'arm state {arm} after activating {vel}')
        speed = min(args.velocity, 1.0)
        drive(ctx.probe, vel, [speed], 1.0)
        ok, message = ctx.probe.switch([pos], [vel])
        if not ok:
            return report('5b mode switch', 'FAIL',
                          f'switch_controllers refused {vel} -> {pos}: {message}\n'
                          f'ros2_control_node:\n{ctx.node.tail(15)}')
        active = pos
        base = ctx.probe.latest_joint()
        p_switch = base['position'][base['name'].index(name)] if base and name in base['name'] else float('nan')
        samples = drive(ctx.probe, pos, None, 1.5)  # NaN: the plugin holds
        s = series(samples, name)
        lowest = min((p for _, p, _ in s), default=float('nan'))
        highest = max((p for _, p, _ in s), default=float('nan'))
        armed, _, arm = ctx.obs.wait_arm(1, 0.5)
        mode = ctx.obs.mode(refs)
        c = component(ctx)
        comp_state = c.state.label if c else 'missing'
        held = bool(s) and lowest >= p_switch - 0.3 and highest <= p_switch + 1.0
        result = report('5b mode switch hold', 'PASS' if held and armed and mode == 'position' and
                        comp_state == 'active' else 'FAIL',
                        f'{vel} -> {pos} while armed at {speed:g} rad/s: position {p_switch:+.3f} rad at the switch, '
                        f'then {lowest:+.3f}..{highest:+.3f} over 1.5 s with no command (need within -0.3/+1.0: no '
                        f'jump back, braking only); arm state {arm} (need 1, no DISARM), mode "{mode}" (need '
                        f'"position"), component {comp_state}',
                        at_switch=p_switch, lowest=lowest, highest=highest, arm_state=arm, mode=mode)
        # Let the hold settle (the wheel braked past the hold and comes back), then step.
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            recent = series(drive(ctx.probe, pos, None, 0.3), name)
            if recent and max(abs(v) for _, _, v in recent) < 0.05:
                break
        start = ctx.probe.latest_joint()
        p0 = start['position'][start['name'].index(name)] if start and name in start['name'] else float('nan')
        t0 = time.monotonic()
        moved = series(drive(ctx.probe, pos, [p0 + 0.5], 3.0), name)
        tail = [p for t, p, _ in moved if t >= t0 + 3.0 - 0.2]
        final = mean(tail) if tail else float('nan')
        # The position controller drives the wheel after the switch. Accuracy is
        # check 3's (from rest): after a mode change while moving, the firmware's
        # integrator preset and the gains shape the step, so only the response
        # is checked here.
        responds = final - p0 >= 0.25 and abs(final - (p0 + 0.5)) <= 0.3
        result &= report('5b mode switch step', 'PASS' if responds else 'FAIL',
                         f'position controller after the switch: {p0:+.3f} -> {final:+.3f} rad after 3 s for a '
                         f'+0.5 rad step (need >= +0.25 and within 0.3 of the target)', start=p0, final=final)
        return result
    finally:
        released_ok, release_message = ctx.probe.switch([], [active])
        state = {'released': released_ok, 'release_message': release_message}
        released(ctx, name, 'position', '5b', state)


def check_take(ctx):
    args = ctx.args
    name = args.joint[0][0]
    ctrl = controller_name(name, 'velocity')
    alive(ctx)
    ok = True
    with claimed(ctx, ctrl) as state:
        if not state['ok']:
            return report('6 GUI Take', 'FAIL', f'switch_controllers refused {ctrl}: {state["message"]}')
        drive(ctx.probe, ctrl, [0.0], 0.5)
        reached, _, arm = ctx.obs.wait_arm(1, 2.0)
        if not reached:
            return report('6 GUI Take', 'FAIL', f'the device is not armed before the Take (arm state {arm})')
        # The CLI plays the GUI: `--lease status N` takes the lease (preempting the plugin) and then
        # streams the device's status until we stop it; its first frame proves the Take completed.
        started = time.monotonic()
        ctx.lease_proc = Proc('cli-lease', ctx.obs.argv('status', 1000000, lease=True), env=ctx.obs.env)
        try:
            ctx.lease_proc.next_line(20, '{')
        except Fail as e:
            return report('6 GUI Take', 'FAIL', f'the second client could not take the lease: {e}')
        took_lease = time.monotonic() - started
        t0 = time.monotonic()
        comp_state = 'active'
        while time.monotonic() - t0 < 1.0:
            c = component(ctx)
            comp_state = c.state.label if c else 'missing'
            if comp_state != 'active':
                break
            time.sleep(0.05)
        gone = time.monotonic() - t0
        ok &= report('6 GUI Take component', 'PASS' if comp_state != 'active' else 'FAIL',
                     f'{gone:.2f} s after the Take completed (it took {took_lease:.2f} s) the component is {comp_state} '
                     f'(need not active within 1 s)', component=comp_state, seconds=gone, take_seconds=took_lease)
        reached, took, arm = ctx.obs.wait_arm(0, 1.5)
        holder = ctx.obs.holder()
        ok &= report('6 GUI Take disarmed', 'PASS' if reached else 'FAIL',
                     f'arm state {arm} {took:.2f} s later (need 0 = disarmed); lease holder {holder}',
                     arm_state=arm, holder=holder)
    # The plugin's component is gone; the claimed() release is expected to fail or be a no-op.
    proc, ctx.lease_proc = ctx.lease_proc, None
    proc.stop()
    deadline = time.monotonic() + 5.0
    holder = ctx.obs.holder()
    while holder and holder[0] != 0 and time.monotonic() < deadline:
        time.sleep(0.2)
        holder = ctx.obs.holder()
    time.sleep(1.0)
    reached, _, arm = ctx.obs.wait_arm(0, 1.0)
    ok &= report('6 GUI Take release', 'PASS' if holder and holder[0] == 0 and arm == 0 else 'FAIL',
                 f'after stopping the CLI the lease holder is {holder} (need nobody), and the device stays '
                 f'disarmed 1 s later (arm state {arm}; nothing re-arms by itself)', holder=holder, arm_state=arm)
    return ok


# ------------------------------------------------------------ run and cleanup

def first_run_procedure(args):
    print('''
FIRST-RUN PROCEDURE: this check drives the motors. Every time:
  1. The robot is on stands, the wheels are OFF the ground.
  2. The supply is current-limited.
  3. The duty cap is low: each controller's duty_limit is at most %g, and the
     duty check commands at most %g.
  4. A hand is on the power switch.
It commands, one wheel at a time: velocity +%g rad/s (2 s), position +%g rad,
duty +%g (1.5 s); then a lease Take by a second client.
''' % (args.duty_cap, min(args.duty, args.duty_cap), args.velocity, args.position_step,
       min(args.duty, args.duty_cap)), flush=True)
    if args.confirm_first_run:
        return report('0 first-run procedure', 'PASS', 'confirmed with --confirm-first-run')
    if not sys.stdin.isatty():
        raise Fail('this check moves the motors: confirm the first-run procedure with --confirm-first-run '
                   '(stdin is not a terminal, nothing was commanded)')
    if input('All four are true. Type "yes" to start: ').strip().lower() != 'yes':
        raise Fail('first-run procedure not confirmed, nothing was commanded')
    return report('0 first-run procedure', 'PASS', 'confirmed at the prompt')


def preflight(ctx):
    """Real hardware: the device is disarmed and every duty_limit is at most --duty-cap."""
    args, obs = ctx.args, ctx.obs
    arm = obs.arm_state()
    if arm != 0:
        raise Fail(f'the device is not disarmed (arm state {arm}): another client is driving it; nothing was changed')
    for name, label in args.joint:
        refs = ctx.refs[name] = obs.resolve(name, label)
        limit = obs.duty_limit(refs)
        if limit <= args.duty_cap + 1e-6:
            continue
        if not args.set_duty_limit:
            raise Fail(f'joint {name}: the controller "{label}" has duty_limit {limit:g}, above --duty-cap '
                       f'{args.duty_cap:g}. Lower it (drivelink-cli --endpoint {args.agent} --device {args.device} '
                       f'set {refs.duty_limit}={args.duty_cap:g}) or pass --set-duty-limit to write it live for '
                       f'this run; nothing was commanded')
        obs.call('set', f'{refs.duty_limit}={args.duty_cap:g}')
        ctx.restore_duty.append((refs.duty_limit, limit))
        print(f'duty_limit of "{label}" set live {limit:g} -> {args.duty_cap:g} (restored at the end)', flush=True)
    report('0 preflight', 'PASS', f'device disarmed; duty_limit of {[l for _, l in args.joint]} at most {args.duty_cap:g}')


def wait_component(ctx, timeout=30):
    """Wait for the component to be active; give up early when it refused to configure or activate.

    Returns None when it is active, else the reason."""
    deadline = time.monotonic() + timeout
    seen = 0
    while time.monotonic() < deadline:
        for line in ctx.node.lines[seen:]:
            if 'configure failed' in line or 'activate failed' in line or 'Failed to set the initial state' in line:
                return line.split(']: ')[-1] if 'failed:' in line else line
        seen = len(ctx.node.lines)
        try:
            for c in ctx.probe.hardware_components(timeout=2):
                if c.name == HW_NAME and c.state.label == 'active':
                    return None
        except Fail:
            pass
        if not ctx.node.alive():
            return 'ros2_control_node exited'
        time.sleep(0.3)
    return f'not active within {timeout} s'


def spawn_inactive(names, yaml_path, node):
    argv = ['ros2', 'run', 'controller_manager', 'spawner', *names, '--inactive', '-c', '/controller_manager',
            '--controller-manager-timeout', '40', '--param-file', str(yaml_path)]
    spawner = Proc('spawner-controllers', argv)
    deadline = time.monotonic() + 90
    while spawner.alive() and time.monotonic() < deadline and node.alive():
        time.sleep(0.2)
    if spawner.alive():
        spawner.stop()
        raise Fail('the controller spawner did not finish within 90 s:\n' + spawner.tail())
    if spawner.p.returncode != 0:
        raise Fail(f'the controller spawner failed ({spawner.p.returncode}):\n{spawner.tail()}\n'
                   f'ros2_control_node:\n{node.tail(40)}')


def spawn_active(name, yaml_path, node):
    spawner = Proc(f'spawner-{name}', ['ros2', 'run', 'controller_manager', 'spawner', name, '-c', '/controller_manager',
                                       '--controller-manager-timeout', '40', '--param-file', str(yaml_path)])
    deadline = time.monotonic() + 60
    while spawner.alive() and time.monotonic() < deadline and node.alive():
        time.sleep(0.2)
    if spawner.alive():
        spawner.stop()
        raise Fail(f'spawner {name} did not finish within 60 s:\n' + spawner.tail())
    if spawner.p.returncode != 0:
        raise Fail(f'spawner {name} failed ({spawner.p.returncode}):\n{spawner.tail()}\n'
                   f'ros2_control_node:\n{node.tail(40)}')


def skip_rest(why):
    for label in ('2 velocity', '3 position', '4 duty', '5 NaN hold', '6 GUI Take'):
        report(label, 'SKIPPED', f'{why}: nothing was commanded')


def run(args, tmp, ctx):
    token = None
    if args.mock:
        token = secrets.token_hex(12)
        os.environ.setdefault('ROS_DOMAIN_ID', str(secrets.randbelow(100) + 1))
        os.environ.setdefault('ROS_AUTOMATIC_DISCOVERY_RANGE', 'LOCALHOST')
        args.agent, args.device = start_mock(args, tmp, token)
        args.joint = [('left_wheel', 'left'), ('right_wheel', 'right')]
    else:
        if not args.joint:
            raise Fail('give at least one --joint NAME=LABEL (or --mock)')
        if not args.device:
            raise Fail('--device ALIAS_OR_ID is required (or --mock)')
        if not args.cli or not os.path.isfile(args.cli):
            raise Fail(f'drivelink-cli not found ({args.cli!r}): pass --cli or set DRIVELINK_CLI')
    if len({n for n, _ in args.joint}) != len(args.joint):
        raise Fail('duplicate joint names')
    if not 0 < args.state_rate <= 10000:
        raise Fail('--state-rate must be in (0, 10000]')
    if args.velocity <= 0 or args.position_step <= 0:
        raise Fail('--velocity and --position-step must be positive')
    if not 0 < args.duty_cap <= 1 or not 0 < args.duty <= 1:
        raise Fail('need 0 < --duty <= 1 and 0 < --duty-cap <= 1')
    if not 0 < args.velocity_tolerance < 1 or args.position_tolerance <= 0:
        raise Fail('need 0 < --velocity-tolerance < 1 and --position-tolerance > 0')

    if not args.mock:
        first_run_procedure(args)
        if token is None:
            token = args.token
        if token is None:
            try:
                token = Path(args.token_file).read_text(encoding='utf-8').splitlines()[0].strip()
            except (OSError, IndexError) as e:
                raise Fail(f'cannot read the agent token from {args.token_file}: {e} '
                           f'(--token, or add the user to the drivelink group)')
    ctx.obs = Observer(args.cli, args.agent, args.device, token)
    ctx.obs.wait_online()
    if args.mock:
        for name, label in args.joint:
            ctx.refs[name] = ctx.obs.resolve(name, label)
        report('0 preflight', 'PASS', f'(mock) device disarmed: arm state {ctx.obs.arm_state()}, '
               f'controllers {[l for _, l in args.joint]} resolved')
    else:
        preflight(ctx)

    urdf, yaml = tmp / 'drivelink.urdf', tmp / 'controllers.yaml'
    urdf.write_text(make_urdf(args, token if args.mock or args.token else None))
    yaml.write_text(make_yaml(args))
    table = controller_table(args)
    ctx.probe = Probe(urdf.read_text(), [n for n, _, _ in table])
    ctx.node = Proc('ros2_control_node', ['ros2', 'run', 'controller_manager', 'ros2_control_node',
                                          '--ros-args', '--params-file', str(yaml)])
    reason = wait_component(ctx)
    if reason is not None:
        report('1 active', 'FAIL', f'the hardware component "{HW_NAME}" did not become active: {reason}\n'
               f'ros2_control_node:\n{ctx.node.tail(12)}')
        skip_rest('check 1 failed')
        return False
    spawn_active('joint_state_broadcaster', yaml, ctx.node)
    spawn_inactive([n for n, _, _ in table], yaml, ctx.node)
    loaded = {c.name: c.state for c in ctx.probe.controllers()}
    missing = [n for n, _, _ in table if loaded.get(n) != 'inactive']
    if missing:
        raise Fail(f'controllers not loaded inactive: {missing} (have {loaded})')
    time.sleep(0.5)

    if not check_active(ctx):
        skip_rest('check 1 failed')
        return False
    ok = run_mode(ctx, '2', 'velocity', body_velocity)
    ok &= run_mode(ctx, '3', 'position', body_position)
    ok &= run_mode(ctx, '4', 'duty', body_duty)
    ok &= check_nan_hold(ctx)
    ok &= check_mode_switch(ctx)
    ok &= check_take(ctx)
    alive(ctx)
    return ok


def shutdown(ctx):
    """Every exit path: deactivate, stop the node, restore, and leave the device disarmed. True when it is."""
    ok = True
    if ctx.lease_proc is not None:
        ctx.lease_proc.stop()
        ctx.lease_proc = None
    if ctx.probe is not None and ctx.node is not None and ctx.node.alive():
        try:
            active = [c.name for c in ctx.probe.controllers(timeout=3)
                      if c.state == 'active' and c.name != 'joint_state_broadcaster']
            if active:
                ctx.probe.switch([], active, timeout=5)
        except Exception as e:
            print(f'cleanup: deactivating the controllers failed: {e}', file=sys.stderr, flush=True)
    if ctx.node is not None:
        ctx.node.stop()   # SIGINT: the plugin releases its lease, the agent disarms
    if ctx.obs is not None:
        # Disarmed first: the duty limit goes back up only on a disarmed device.
        done, text = ctx.obs.ensure_disarmed()
        ok &= report('cleanup device disarmed', 'PASS' if done else 'FAIL', text)
        if not done:
            print('!!! THE DEVICE MAY STILL BE ARMED: CUT THE POWER !!!', file=sys.stderr, flush=True)
            if ctx.restore_duty:
                print('cleanup: duty_limit left at the cap (not restored on an armed device)', file=sys.stderr,
                      flush=True)
        else:
            for ref, value in reversed(ctx.restore_duty):
                try:
                    ctx.obs.call('set', f'{ref}={value:g}')
                except Fail as e:
                    ok = False
                    print(f'cleanup: could not restore duty_limit {ref}={value:g}: {e}', file=sys.stderr, flush=True)
    if ctx.probe is not None:
        ctx.probe.close()
    return ok


def main():
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--agent', default='tcp://127.0.0.1:47620')
    ap.add_argument('--device', help='device alias or 12-hex id behind the agent')
    ap.add_argument('--token')
    ap.add_argument('--token-file', default='/etc/drivelink/token')
    ap.add_argument('--joint', type=parse_pair, action='append', default=[], metavar='NAME=LABEL')
    ap.add_argument('--state-rate', type=float, default=100, help='controller_manager update_rate and state_rate_hz')
    ap.add_argument('--velocity', type=float, default=2.0, help='rad/s of the velocity check')
    ap.add_argument('--velocity-tolerance', type=float, default=0.3, help='fraction of --velocity (0.3 = 30%%)')
    ap.add_argument('--position-step', type=float, default=1.0, help='rad of the position check')
    ap.add_argument('--position-tolerance', type=float, default=0.15, help='rad')
    ap.add_argument('--duty', type=float, default=0.15, help='duty of the duty check (capped by --duty-cap)')
    ap.add_argument('--duty-cap', type=float, default=0.2, help='highest duty this run may command or allow')
    ap.add_argument('--confirm-first-run', action='store_true',
                    help='the first-run procedure holds: wheels off the ground, current-limited supply, low duty '
                         'cap, hand on the power switch (skips the prompt)')
    ap.add_argument('--set-duty-limit', action='store_true',
                    help="write each controller's duty_limit down to --duty-cap live (not saved) for this run")
    ap.add_argument('--report', help='write every measured value and verdict here as JSON')
    ap.add_argument('--mock', action='store_true', help='run against drivelink-vdev behind drivelink-agent')
    ap.add_argument('--vdev', default=env('DRIVELINK_VDEV'))
    ap.add_argument('--agent-bin', dest='agent_bin', default=env('DRIVELINK_AGENT'))
    ap.add_argument('--cli', default=env('DRIVELINK_CLI') or shutil.which('drivelink-cli'))
    args = ap.parse_args()

    started = time.monotonic()
    ok = False
    ctx = Context(args)

    def interrupt(*_):
        raise KeyboardInterrupt()
    # explicit, so Ctrl-C and SIGTERM also work when started in the background (SIGINT ignored)
    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    with tempfile.TemporaryDirectory(prefix='drivelink-ros-command-') as tmp:
        try:
            print(f'plugin package: {have_ros()}', flush=True)
            ok = run(args, Path(tmp), ctx)
        except Fail as failure:
            print(f'FAIL {failure}', file=sys.stderr, flush=True)
            RESULTS.append({'check': 'setup', 'verdict': 'FAIL', 'detail': str(failure), 'values': {}})
        except KeyboardInterrupt:
            print('interrupted', file=sys.stderr, flush=True)
            RESULTS.append({'check': 'run', 'verdict': 'FAIL', 'detail': 'interrupted', 'values': {}})
        finally:
            # The cleanup is bounded; a second Ctrl-C must not cut it short.
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                ok = shutdown(ctx) and ok
            except Exception as e:
                ok = False
                print(f'FAIL cleanup raised {e!r}', file=sys.stderr, flush=True)
            stop_all()
    verdict = 'PASS' if ok else 'FAIL'
    print(f'{verdict} ros command check ({len(RESULTS)} results, {time.monotonic() - started:.0f} s)', flush=True)
    if args.report:
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps({
            'verdict': verdict, 'mock': args.mock, 'agent': args.agent, 'device': args.device,
            'joints': args.joint, 'velocity': args.velocity, 'velocity_tolerance': args.velocity_tolerance,
            'position_step': args.position_step, 'position_tolerance': args.position_tolerance,
            'duty': min(args.duty, args.duty_cap), 'duty_cap': args.duty_cap,
            'checks': RESULTS}, indent=2) + '\n')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
