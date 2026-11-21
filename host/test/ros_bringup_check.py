#!/usr/bin/env python3
"""Run the installed example launch against vdev (--mock) or a prepared robot."""
import argparse
import json
import math
import os
from pathlib import Path
import secrets
import shutil
import signal
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

from ros_command_check import MOCK_GAINS, Observer, Probe
from ros_state_check import Fail, Proc, RESULTS, report, start_mock, stop_all


DRIVE = 'diff_drive_controller'
BROADCASTERS = ('joint_state_broadcaster', 'imu_sensor_broadcaster')
LEFT, RIGHT = 'left_wheel_joint', 'right_wheel_joint'
MOCK_WHEEL_SEPARATION = 0.42


class BringupProbe(Probe):
    def __init__(self):
        from diagnostic_msgs.msg import DiagnosticArray
        from geometry_msgs.msg import TwistStamped
        from nav_msgs.msg import Odometry
        from sensor_msgs.msg import Imu
        from rclpy.qos import DurabilityPolicy, QoSProfile, qos_profile_sensor_data
        from std_msgs.msg import String
        self.odom = self.imu = None
        self.description = None
        self.diagnostic = {}
        super().__init__(None, [])
        self.twist_type = TwistStamped
        self.cmd = self.node.create_publisher(TwistStamped, '/cmd_vel', 10)
        self.node.create_subscription(Odometry, '/odom', self.on_odom, qos_profile_sensor_data)
        self.node.create_subscription(Imu, '/imu', self.on_imu, qos_profile_sensor_data)
        self.node.create_subscription(DiagnosticArray, '/diagnostics', self.on_diagnostic, 10)
        self.node.create_subscription(String, '/robot_description', self.on_description,
                                      QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL))

    def on_odom(self, msg):
        self.odom = msg

    def on_imu(self, msg):
        self.imu = msg

    def on_description(self, msg):
        self.description = msg.data

    def on_diagnostic(self, msg):
        for status in msg.status:
            if status.name.startswith('DriveLink '):
                self.diagnostic = {kv.key: kv.value for kv in status.values}

    def trigger(self, service, node='drivelink_diagnostics'):
        from std_srvs.srv import Trigger
        return self.call(Trigger, f'/{node}/{service}', Trigger.Request(), 90)

    def command(self, linear=0.0, angular=0.0):
        msg = self.twist_type()
        msg.header.stamp = self.node.get_clock().now().to_msg()
        msg.twist.linear.x = linear
        msg.twist.angular.z = angular
        self.cmd.publish(msg)

    def wheels(self, field):
        joint = self.latest_joint()
        if joint and all(name in joint['name'] for name in (LEFT, RIGHT)):
            values = joint[field]
            if len(values) == len(joint['name']):
                return tuple(values[joint['name'].index(name)] for name in (LEFT, RIGHT))
        return (math.nan, math.nan)


def require(ok, text):
    if not ok:
        raise Fail(text)


def wait_for(predicate, description, seconds=10):
    deadline = time.monotonic() + seconds
    last_error = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except Fail as error:
            last_error = error
        time.sleep(0.05)
    detail = f' ({last_error})' if last_error is not None else ''
    raise Fail(f'timed out waiting for {description}{detail}')


def controllers_ready(probe, drive_state):
    states = {c.name: c.state for c in probe.controllers(timeout=2)}
    return states.get(DRIVE) == drive_state and all(states.get(c) == 'active' for c in BROADCASTERS)


def component_state(probe):
    return next((c.state.label for c in probe.hardware_components(timeout=2) if c.name == 'drivelink'), 'missing')


def stream(probe, linear=0.0, angular=0.0, seconds=1.5):
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            probe.command(linear, angular)
            time.sleep(0.05)
    finally:
        probe.command()


def drive(probe):
    wait_for(lambda: probe.odom is not None and all(map(math.isfinite, probe.wheels('position'))),
             'odometry and wheel positions')
    before_odom, before_wheels = probe.odom.pose.pose.position, probe.wheels('position')
    stream(probe, linear=0.10)
    stream(probe, seconds=0.5)
    after_odom, after_wheels = probe.odom.pose.pose.position, probe.wheels('position')
    distance = math.hypot(after_odom.x - before_odom.x, after_odom.y - before_odom.y)
    steps = [after - before for before, after in zip(before_wheels, after_wheels)]
    require(distance > 0.04 and all(step > 0.4 for step in steps),
            f'forward command produced {distance:.3f} m and wheel changes {steps} rad')
    return distance


def quaternion(imu):
    q = imu.orientation
    return q.x, q.y, q.z, q.w


def yaw(imu):
    x, y, z, w = quaternion(imu)
    return math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))


def write_scenario(path, seq, action):
    stage = path.with_suffix('.tmp')
    stage.write_text(f'{seq} {action}\n', encoding='ascii')
    stage.replace(path)


def verify_error(probe):
    wait_for(lambda: component_state(probe) == 'unconfigured', 'hardware ERROR (unconfigured)', 8)
    wait_for(lambda: any(c.name == DRIVE and c.state == 'inactive' for c in probe.controllers(timeout=2)),
             'inactive drive controller')


def require_disarmed(obs):
    reached, _, state = obs.wait_arm(0, 5)
    require(reached, f'expected disarmed, arm state was {state}')


def mock_controller_config(tmp):
    from ament_index_python.packages import get_package_share_directory
    import yaml
    source = Path(get_package_share_directory('drivelink_bringup')) / 'config' / 'controllers.yaml'
    config = yaml.safe_load(source.read_text(encoding='utf-8'))
    config[DRIVE]['ros__parameters']['wheel_separation'] = MOCK_WHEEL_SEPARATION
    path = tmp / 'controllers.yaml'
    path.write_text(yaml.safe_dump(config), encoding='utf-8')
    return path


def check_mock_geometry(probe):
    from rcl_interfaces.srv import GetParameters
    request = GetParameters.Request()
    request.names = ['wheel_separation']
    actual = probe.call(GetParameters, f'/{DRIVE}/get_parameters', request, 3).values[0].double_value
    wait_for(lambda: probe.description is not None, 'robot description')
    robot = ET.fromstring(probe.description)
    offsets = [float(robot.find(f"./joint[@name='{joint}']/origin").attrib['xyz'].split()[1])
               for joint in (LEFT, RIGHT)]
    require(math.isclose(actual, MOCK_WHEEL_SEPARATION) and math.isclose(offsets[0] - offsets[1], actual),
            f'wheel separation differs between the controller ({actual:g}) and URDF ({offsets})')
    report('wheel geometry', 'PASS', f'controller and URDF agree on {actual:g} m')


def check_failed_recovery(args, probe, token_file):
    from controller_manager_msgs.msg import ControllerState, HardwareComponentState, HardwareInterface
    from controller_manager_msgs.srv import (ListControllers, ListHardwareComponents,
                                              SetHardwareComponentState, SwitchController)
    from lifecycle_msgs.msg import State

    manager = '/h4c_test_manager'
    node = 'h4c_test_diagnostics'
    inactive, active = State.PRIMARY_STATE_INACTIVE, State.PRIMARY_STATE_ACTIVE
    labels = {inactive: 'inactive', active: 'active'}
    hw = inactive
    states = {DRIVE: 'inactive', BROADCASTERS[0]: 'inactive', BROADCASTERS[1]: 'active'}
    moves, switches = [], []
    claim = HardwareInterface(name=f'{LEFT}/velocity', is_available=True)

    def list_hardware(_, response):
        response.component = [HardwareComponentState(name='drivelink', state=State(id=hw, label=labels[hw]),
                                                     command_interfaces=[claim])]
        return response

    def list_controllers(_, response):
        response.controller = [ControllerState(name=name, state=state) for name, state in states.items()]
        return response

    def set_hardware(request, response):
        nonlocal hw
        hw = request.target_state.id
        moves.append(hw)
        if len(moves) == 1:
            # The first request lands, but its response arrives after the client deadline.
            time.sleep(30.5)
        response.ok = True
        response.state = State(id=hw, label=labels[hw])
        return response

    def switch(request, response):
        changes = (list(request.activate_controllers), list(request.deactivate_controllers))
        switches.append(changes)
        if changes[0]:
            states[DRIVE] = 'active'
            response.ok = False
            response.message = 'simulated partial controller switch'
        else:
            for name in changes[1]:
                states[name] = 'inactive'
            response.ok = True
        return response

    services = [probe.node.create_service(kind, manager + '/' + name, callback)
                for kind, name, callback in (
                    (ListHardwareComponents, 'list_hardware_components', list_hardware),
                    (ListControllers, 'list_controllers', list_controllers),
                    (SetHardwareComponentState, 'set_hardware_component_state', set_hardware),
                    (SwitchController, 'switch_controller', switch))]
    worker = Proc('recovery rollback', ['ros2', 'run', 'drivelink_bringup', 'diagnostics',
                  '--ros-args', '-r', f'__node:={node}', '-p', f'controller_manager:={manager}',
                  '-p', f'agent:={args.agent}', '-p', f'device:={args.device}', '-p', f'token_file:={token_file}',
                  '-p', f'recovery_controllers:=[{DRIVE},{BROADCASTERS[0]}]'])
    try:
        wait_for(lambda: probe.diagnostic.get('online') == 'true', 'rollback observer online')
        response = probe.trigger('recover', node)
        require(not response.success and 'outcome unknown' in response.message and
                moves == [active, inactive] and not switches, response.message)
        response = probe.trigger('recover', node)
        require(not response.success and 'simulated partial' in response.message and
                moves == [active, inactive, active, inactive] and
                switches == [([DRIVE, BROADCASTERS[0]], []), ([], [DRIVE])] and
                states == {DRIVE: 'inactive', BROADCASTERS[0]: 'inactive', BROADCASTERS[1]: 'active'},
                response.message)
        claim.is_claimed = True
        response = probe.trigger('recover', node)
        require(not response.success and 'still claimed' in response.message and len(moves) == 4 and len(switches) == 2,
                response.message)
        report('failed recovery rollback', 'PASS',
               'late activation reply and partial switch rolled back; a residual claim blocked retry')
    except Fail:
        print(worker.tail(30), file=sys.stderr)
        raise
    finally:
        worker.stop()
        for service in services:
            probe.node.destroy_service(service)


def recover(args, probe, obs):
    if not args.mock:
        require(input('Leave room to drive forward. Type yes to re-arm and continue: ').strip().lower() == 'yes',
                'recovery drive was not confirmed')
    response = probe.trigger('recover')
    require(response.success, response.message)
    wait_for(lambda: controllers_ready(probe, 'active'), 'configured controllers active')
    wait_for(lambda: obs.arm_state() == 1, 'armed after explicit recovery')
    time.sleep(0.4)
    require(all(abs(v) < 0.2 for v in probe.wheels('velocity')), 'wheels did not hold after recovery')
    report('recover', 'PASS', f'held until a fresh cmd_vel; traveled {drive(probe):.3f} m afterwards')


def check_fault(args, probe, obs, scenario):
    if args.mock:
        def moving():
            probe.command(linear=0.10)
            return all(v > 0.4 for v in probe.wheels('velocity'))

        wait_for(moving, 'wheels moving before the injected fault')
        write_scenario(scenario, 1, 'fault')
        wait_for(lambda: probe.diagnostic.get('arm_state') == '2' and
                 probe.diagnostic.get('last_event_code') == str(0x0104), 'fault and LoopStalled diagnostic')
    else:
        # A deliberate device DISARM exercises the component's disarm/ERROR path
        # without inducing a physical fault or stalling a real motor.
        obs.call('disarm')
    verify_error(probe)
    wait_for(lambda: probe.diagnostic.get('lease_holder_id') == '0', 'ROS lease release after ERROR')
    if args.mock:
        for service in ('ack_fault', 'recover'):
            response = probe.trigger(service)
            require(not response.success and 'detail=43' in response.message, response.message)
        require(component_state(probe) == 'unconfigured', 'active fault changed hardware state')
        write_scenario(scenario, 2, 'fault-clear')
        wait_for(lambda: probe.diagnostic.get('active_errors') == '0', 'fault condition cleared')
        require(obs.arm_state() == 2, 'clearing the cause unexpectedly cleared the fault latch')
    report('fault', 'PASS', 'component entered ERROR and released its lease')
    recover(args, probe, obs)


def check_take(args, probe, obs):
    actor = None
    if args.mock:
        actor = Proc('cli-lease', obs.argv('status', 1000000, lease=True), env=obs.env)
        actor.next_line(20, '{')
    else:
        input('In the GUI, Take the lease without arming. Press Enter once it is held... ')
    try:
        verify_error(probe)
        require_disarmed(obs)
        wait_for(lambda: probe.diagnostic.get('lease_holder_kind') == ('2' if args.mock else '1'),
                 'second client shown as lease holder')
        response = probe.trigger('recover')
        require(not response.success and 'lease held by' in response.message, response.message)
        require(component_state(probe) == 'unconfigured', 'recovery changed hardware while another client held the lease')
    finally:
        if actor:
            actor.stop()
    if not args.mock:
        input('Release the GUI lease and press Enter... ')
    wait_for(lambda: probe.diagnostic.get('lease_holder_id') == '0', 'second client lease release')
    response = probe.trigger('ack_fault')
    require(response.success, response.message)
    require_disarmed(obs)
    require(component_state(probe) == 'unconfigured', 'ack_fault activated hardware')
    report('GUI-style Take', 'PASS', 'component entered ERROR and stayed disarmed after the lease was released')
    recover(args, probe, obs)


def run(args, tmp):
    scenario = tmp / 'scenario'
    if 'microsoft' in os.uname().release.lower() and not Path('/.dockerenv').exists():
        os.environ.setdefault('FASTDDS_BUILTIN_TRANSPORTS', 'SHM')
    os.environ.setdefault('ROS_DOMAIN_ID', str(secrets.randbelow(100) + 1))
    os.environ.setdefault('ROS_AUTOMATIC_DISCOVERY_RANGE', 'LOCALHOST')
    require(args.cli, 'set --cli or DRIVELINK_CLI to the drivelink-cli executable')
    if args.mock:
        token = secrets.token_hex(16)
        args.agent, args.device = start_mock(args, tmp, token, gains=MOCK_GAINS, scenario=scenario)
        token_file = tmp / 'token'
        token_file.write_text(token + ' \t\r\n', encoding='ascii')
        token_file.chmod(0o600)
        args.controllers_file = mock_controller_config(tmp)
    else:
        token_file = Path(args.token_file)
        with token_file.open(encoding='utf-8') as config:
            token = config.readline().rstrip(' \t\r\n')
        require(token, f'agent token file has an empty first line: {token_file}')
        require(sys.stdin.isatty(), 'real robot check requires a terminal for the first-run procedure')
        print('FIRST RUN: wheels off the ground on stands, current-limited supply, duty limit at most 0.2, '
              'and a hand on the power switch.', flush=True)
        require(input('Type yes when all four are true: ').strip().lower() == 'yes', 'first run was not confirmed')
    obs = Observer(args.cli, args.agent, args.device, token)
    obs.wait_online()
    require(obs.arm_state() == 0 and (obs.holder() or (0,))[0] == 0, 'device must be disarmed with the lease free')
    if not args.mock:
        for joint, label in ((LEFT, args.left_label), (RIGHT, args.right_label)):
            limit = obs.duty_limit(obs.resolve(joint, label))
            require(0 < limit <= 0.2 + 1e-6, f'{label}: duty_limit {limit:g} must be positive and at most 0.2')

    command = ['ros2', 'launch', 'drivelink_bringup', 'example.launch.py',
               f'agent:={args.agent}', f'device:={args.device}', f'token_file:={token_file}',
               f'left_label:={args.left_label}', f'right_label:={args.right_label}', f'imu_label:={args.imu_label}']
    if args.controllers_file:
        command.append(f'controllers_file:={args.controllers_file}')
    probe = BringupProbe()
    launch = None
    try:
        if args.mock:
            check_failed_recovery(args, probe, token_file)
        launch = Proc('bringup', command)
        wait_for(lambda: controllers_ready(probe, 'inactive'), 'state broadcasters and inactive drive', 50)
        require_disarmed(obs)
        wait_for(lambda: probe.diagnostic.get('online') == 'true' and probe.imu is not None and
                 all(map(math.isfinite, probe.wheels('position'))), 'diagnostics, joint states and IMU')
        require(abs(math.hypot(*quaternion(probe.imu)) - 1.0) < 0.02, 'IMU quaternion is not normalized')
        report('launch', 'PASS', 'diagnostics and state topics live, device disarmed with drive controller inactive')
        if args.mock:
            check_mock_geometry(probe)
        ok, detail = probe.switch([DRIVE], [])
        require(ok, detail)
        wait_for(lambda: obs.arm_state() == 1, 'ARM after explicit drive controller activation')
        distance = drive(probe)
        report('forward on stands' if not args.mock else 'mock forward', 'PASS', f'odometry advanced {distance:.3f} m')
        if not args.mock:
            ok, detail = probe.switch([], [DRIVE])
            require(ok, detail)
            require_disarmed(obs)
            require(input('Verify both wheels turned forward. Set the robot on a clear floor, then type yes: ')
                    .strip().lower() == 'yes', 'floor check was not confirmed')
            ok, detail = probe.switch([DRIVE], [])
            require(ok, detail)
            distance = drive(probe)
            measured = float(input('Enter the measured forward distance in metres: '))
            require(0 < measured and abs(distance - measured) <= max(0.03, 0.2 * measured),
                    f'odometry {distance:.3f} m differs from measured {measured:.3f} m')
            before = yaw(probe.imu)
            stream(probe, angular=0.25)
            delta = math.remainder(yaw(probe.imu) - before, 2 * math.pi)
            require(0.12 < delta < 0.70, f'positive yaw command changed IMU heading by {delta:.3f} rad')
            report('floor odometry and heading', 'PASS', f'measured {measured:.3f} m, odom {distance:.3f} m, yaw {delta:.3f} rad')
        check_fault(args, probe, obs, scenario)
        check_take(args, probe, obs)
    except (Fail, ValueError, OSError):
        if launch:
            print(launch.tail(70), file=sys.stderr)
        raise
    finally:
        try:
            probe.command()
        finally:
            probe.close()
            if launch:
                launch.stop()
                try:
                    require_disarmed(obs)
                    report('shutdown', 'PASS', 'device is disarmed after launch exit')
                except Fail as error:
                    report('shutdown', 'FAIL', str(error))
                    disarmed, detail = obs.ensure_disarmed()
                    print(detail if disarmed else f'{detail}. Switch off motor power now.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mock', action='store_true')
    parser.add_argument('--agent', default='tcp://127.0.0.1:47620')
    parser.add_argument('--device', default='dev')
    parser.add_argument('--token-file', default='/etc/drivelink/token')
    parser.add_argument('--left-label', default='left')
    parser.add_argument('--right-label', default='right')
    parser.add_argument('--imu-label', default='imu')
    parser.add_argument('--controllers-file', type=Path)
    parser.add_argument('--cli', default=os.getenv('DRIVELINK_CLI') or shutil.which('drivelink-cli'))
    parser.add_argument('--vdev', default=os.getenv('DRIVELINK_VDEV'))
    parser.add_argument('--agent-bin', default=os.getenv('DRIVELINK_AGENT'))
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    args.state_rate = 100

    def interrupt(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    with tempfile.TemporaryDirectory(prefix='drivelink-bringup-') as tmp:
        try:
            run(args, Path(tmp))
        except (Fail, ValueError, OSError, KeyboardInterrupt, EOFError) as error:
            report('bringup check', 'FAIL', str(error) or 'interrupted')
        finally:
            stop_all()
    if args.report:
        args.report.write_text(json.dumps(RESULTS, indent=2) + '\n', encoding='utf-8')
    return int(any(result['verdict'] == 'FAIL' for result in RESULTS))


if __name__ == '__main__':
    sys.exit(main())
