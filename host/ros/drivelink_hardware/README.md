# drivelink_hardware

A ros2_control `SystemInterface` (`drivelink_hardware/DriveLinkSystem`) for one
DriveLink device, always reached through `drivelink-agent`: the plugin never
opens a serial port, so the agent keeps owning the robot's ports while ROS is
down and the GUI can share the device. One hardware component per device.
Built for ROS 2 Lyrical (Ubuntu 26.04) and Jazzy (Ubuntu 24.04) with the API
both share. The design is [host-architecture.md](../../../plans/host-architecture.md),
"`drivelink_hardware` (ROS 2)".

**H4a: state interfaces only.** Joints export `position`, `velocity`, `duty`
(or any other signal), sensors the IMU's orientation, angular velocity and
linear acceleration. Command interfaces, ARM and the mode switch arrive in
H4b; a `<command_interface>` is refused at configure until then.

## Build

From a colcon workspace that holds this repository (for example
`~/ros_ws/src/drivelink_v2`), with ROS sourced:

```sh
sudo sh src/drivelink_v2/tools/setup_linux.sh ros   # once: ros2_control, ros2_controllers, colcon
source /opt/ros/$ROS_DISTRO/setup.bash
colcon build --packages-up-to drivelink_hardware
source install/setup.bash
```

colcon builds `drivelink_protocol` (`protocol/`) and `drivelink`
(`host/libdrivelink/`) first; the rest of the repository carries
`COLCON_IGNORE`. CMake downloads the pinned Asio and mdns sources on the first
configure. `tools/validate.py ros` (WSL, Lyrical) and `ros-jazzy` (Docker)
build and test it the same way ([development.md](../../../docs/development.md)).

## The `<ros2_control>` block

```xml
<ros2_control name="drivelink" type="system">
  <hardware>
    <plugin>drivelink_hardware/DriveLinkSystem</plugin>
    <param name="device">robot</param>
    <!-- optional, with their defaults:
    <param name="agent">tcp://127.0.0.1:47620</param>
    <param name="token_file">/etc/drivelink/token</param>
    <param name="take_lease">refuse</param>
    <param name="state_rate_hz">100</param>
    <param name="stale_timeout_ms">auto</param>
    <param name="timeout_ms">3000</param> -->
  </hardware>
  <joint name="left_wheel_joint">
    <param name="instance">left</param>
    <state_interface name="position"/>
    <state_interface name="velocity"/>
  </joint>
  <sensor name="imu">
    <param name="instance">imu</param>
    <state_interface name="orientation.x"/> <state_interface name="orientation.y"/>
    <state_interface name="orientation.z"/> <state_interface name="orientation.w"/>
    <state_interface name="angular_velocity.x"/> <state_interface name="angular_velocity.y"/>
    <state_interface name="angular_velocity.z"/>
    <state_interface name="linear_acceleration.x"/> <state_interface name="linear_acceleration.y"/>
    <state_interface name="linear_acceleration.z"/>
  </sensor>
</ros2_control>
```

### Hardware parameters

| Parameter | Default | Meaning |
|---|---|---|
| `device` | required | The agent's alias of the device, or its 12-hex device id |
| `agent` | `tcp://127.0.0.1:47620` | The agent's address; a host name is resolved |
| `token_file` | `/etc/drivelink/token` | The agent's token, first line. The packaged agent keeps it `0640 root:drivelink`: add the ROS user to the `drivelink` group (`sudo usermod -aG drivelink $USER`, then log in again) |
| `token` | empty | The token inline, for tests and vdev; wins over `token_file` |
| `take_lease` | `refuse` | `refuse`: activation fails, naming the holder, while another client (the GUI tuning) holds the lease. `preempt`: take it anyway; the agent disarms the device and clears the old holder's command groups first |
| `state_rate_hz` | `100` | How often the plugin needs fresh state, normally the controller_manager's `update_rate`. Each loop's group runs at its loop rate divided by `max(1, floor(loop rate / state_rate_hz))`, so it is never slower than this and spends no more of the link than needed |
| `stale_timeout_ms` | `auto` | `read()` returns `ERROR` when a group's newest sample is older than this. `auto`: ten group periods, at least 100 ms; `0` turns the check off |
| `timeout_ms` | `3000` | Each agent and device request while configuring and activating |

An unknown parameter name is refused, so a typo never falls back to a default.

### Joints and sensors

Each `<joint>` or `<sensor>` names the **label** of its device instance with
`<param name="instance">`; the instance must be configured and running.
Each state interface reads one signal of that instance:

| Component | Interface | Signal |
|---|---|---|
| joint | `position`, `velocity`, `duty`, any other name | the signal of the same name |
| sensor | `orientation.w/x/y/z` | `quaternion.w/x/y/z` |
| sensor | `angular_velocity.x/y/z` | `gyro.x/y/z` (rad/s) |
| sensor | `linear_acceleration.x/y/z` | `accel.x/y/z` (m/s², gravity included, as `sensor_msgs/Imu` expects) |
| sensor | any other name | the signal of the same name |

`<param name="state.INTERFACE">SIGNAL</param>` overrides one interface's
signal, so another controller type or naming needs no code. `duty` is a custom
interface (open-loop duty, -1..1), not `effort`. The IMU's interfaces are
published with `imu_sensor_broadcaster`.

## Lifecycle

- **configure**: reads the token, connects to the agent (its Hello says
  `client_kind` Ros, so the GUI shows "ROS is driving"), loads the device
  model and resolves every label and signal. Safe mode, a pending reboot, an
  invalid layout, a missing or failed instance, a missing signal or one of the
  wrong kind or type fails with a message naming the joint, the interface and
  what was found.
- **activate**: refuses while another client holds the lease (unless
  `take_lease` is `preempt`), takes the lease, and sets up one subscription
  group per loop that holds a mapped signal; it waits for the first sample of
  each. State-only use never arms.
- **read()** (real time: `noexcept`, no allocation, blocking lock or device call)
  copies the newest sample of each group into the state interfaces. It returns
  `ERROR` when the lease is lost (another client took it), the agent reports
  the device offline or shuts down, the connection drops, the device reboots,
  or a group's newest sample is older than `stale_timeout_ms` (by the
  sample's device time mapped through the agent's clock model). ros2_control
  then calls `on_error`, which logs the reason, clears the groups, releases the
  lease and disconnects: the component is unconfigured, and nothing comes back
  by itself. Configure and activate it again
  (`ros2 control set_hardware_component_state drivelink active`) once the
  cause is fixed; H4c adds the diagnostics node's `recover`.
- **deactivate**: clears the groups and releases the lease.

## Tests

`test_mapping` covers the parameters, the conventions and every refusal
(no ROS, no device). `test_integration` (built when `DRIVELINK_HW_INTEGRATION`
is set, as `tools/ros_check.sh` does) runs the plugin through
`hardware_interface::ResourceManager` against `drivelink-vdev` behind
`drivelink-agent`: configure and activate, values following the device, the
lease refusal and `preempt`, and `ERROR` on a lost lease, a stopped agent and
stale data. `host/test/ros_state_check.py` is the robot check (wheels turned by
hand, the IMU tilted) with a `--mock` mode against vdev.
