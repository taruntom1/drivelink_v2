import math
from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction, RegisterEventHandler
from launch.event_handlers import OnProcessExit
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
import xacro
import yaml


def launch_robot(context, share):
    def arg(name):
        return LaunchConfiguration(name).perform(context)

    connection = {name: arg(name) for name in ('agent', 'device', 'token_file', 'token')}
    mapping = dict(connection, **{name: arg(name) for name in
                                ('left_label', 'right_label', 'imu_label')})
    config = arg('controllers_file')
    with open(config, encoding='utf-8') as stream:
        params = yaml.safe_load(stream)['diff_drive_controller']['ros__parameters']
    separation = float(params['wheel_separation'])
    if not math.isfinite(separation) or separation <= 0:
        raise ValueError('diff_drive_controller wheel_separation must be positive and finite')
    mapping['wheel_separation'] = str(separation)
    description = xacro.process_file(str(share / 'urdf' / 'example.urdf.xacro'), mappings=mapping).toxml()
    controllers = [name.strip() for name in arg('recovery_controllers').split(',') if name.strip()]
    if not controllers:
        raise ValueError('recovery_controllers must name at least one controller')
    robot = Node(package='robot_state_publisher', executable='robot_state_publisher',
                 parameters=[{'robot_description': ParameterValue(description, value_type=str)}], output='screen')
    manager = Node(package='controller_manager', executable='ros2_control_node',
                   parameters=[config], output='screen')
    diagnostics_params = {name: ParameterValue(value, value_type=str) for name, value in connection.items()}
    diagnostics = Node(package='drivelink_bringup', executable='diagnostics', output='screen',
                       parameters=[dict(diagnostics_params, hardware='drivelink', recovery_controllers=controllers)])
    state = Node(package='controller_manager', executable='spawner', output='screen',
                 arguments=['joint_state_broadcaster', 'imu_sensor_broadcaster',
                            '--param-file', config,
                            '--controller-ros-args', '--ros-args -r /imu_sensor_broadcaster/imu:=/imu',
                            '--controller-manager-timeout', '30'])
    drive = Node(package='controller_manager', executable='spawner', output='screen',
                 arguments=['diff_drive_controller', '--inactive', '--controller-manager-timeout', '30',
                            '--param-file', config,
                            '--controller-ros-args', '--ros-args -r /diff_drive_controller/cmd_vel:=/cmd_vel '
                            '-r /diff_drive_controller/odom:=/odom'])
    # Keep the spawners from racing for the controller_manager's switch service.
    after_state = RegisterEventHandler(OnProcessExit(
        target_action=state, on_exit=lambda event, _: [drive] if event.returncode == 0 else []))
    return [after_state, robot, manager, diagnostics, state]


def generate_launch_description():
    share = Path(get_package_share_directory('drivelink_bringup'))
    defaults = {
        'agent': 'tcp://127.0.0.1:47620', 'device': 'dev', 'token_file': '/etc/drivelink/token', 'token': '',
        'left_label': 'left', 'right_label': 'right', 'imu_label': 'imu',
        'controllers_file': str(share / 'config' / 'controllers.yaml'),
        'recovery_controllers': 'joint_state_broadcaster,imu_sensor_broadcaster,diff_drive_controller',
    }
    return LaunchDescription([
        *(DeclareLaunchArgument(name, default_value=value) for name, value in defaults.items()),
        OpaqueFunction(function=launch_robot, args=[share]),
    ])
