# drivelink_hardware

A ros2_control `SystemInterface` (`drivelink_hardware/DriveLinkSystem`) for one
DriveLink device, always reached through `drivelink-agent`: the plugin never
opens a serial port, so the agent keeps owning the robot's ports while ROS is
down and the GUI can share the device. One hardware component per device.
Built for ROS 2 Lyrical (Ubuntu 26.04) and Jazzy (Ubuntu 24.04) with the API
both share. The design is [host-architecture.md](../../../plans/host-architecture.md),
"`drivelink_hardware` (ROS 2)".

Joints export state interfaces (`position`, `velocity`, `duty` or any other
signal) and the command interfaces `position`, `velocity` and `duty`; sensors
export the IMU's orientation, angular velocity and linear acceleration. A
controller claiming a command interface sets the joint's controller mode and
arms the device; the last release disarms it (H4b).

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
    <command_interface name="velocity"/>
    <command_interface name="position"/>
    <command_interface name="duty"/>
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
| `state_rate_hz` | `100` | 1..10000 Hz: how often the plugin needs fresh state, normally the controller_manager's `update_rate`. Each loop's group runs at its loop rate divided by `max(1, floor(loop rate / state_rate_hz))`, so it is never slower than this and spends no more of the link than needed |
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

### Command interfaces

Joints only, and only these three; each writes one input of the instance and
claiming it writes the instance's `mode` param:

| Interface | Input | Mode entry |
|---|---|---|
| `position` | `position_sp` (rad) | `position` |
| `velocity` | `velocity_sp` (rad/s) | `velocity` |
| `duty` | `duty_sp` (-1..1, open loop) | `open_loop` |

Joint params override the defaults: `command.INTERFACE` (the input item),
`mode.INTERFACE` (the enum entry), and `mode_param` (the mode param's name,
default `mode`; `none` writes no mode). A controller type without a `mode`
param gets no mode writes. A `position` command needs the joint's measured
position for its hold (`state.position`, else the signal `position`); it is
subscribed even when the joint exports no `position` state interface. The
command interfaces' `data_type` must be double, every input must be an `f32`
input, and no two command interfaces may name the same input.

- **One command interface per joint at a time**; joints may be in different
  modes. A switch that claims two interfaces of one joint, or claims a joint
  whose current interface is not released, fails.
- **Arming.** The first claim arms the device: the mode is written, then ARM,
  and the switch waits until the status stream shows the device armed
  (about two status periods, 0.2 s at the default 10 Hz). A refused ARM fails
  the switch and the log names its result and detail (a latched fault, a
  module blocking arming, ...). An ARM without an answer is followed by a
  DISARM and fails the switch. The last release disarms the device. A mode
  change while armed needs no DISARM: the firmware switches bumplessly and
  holds the joint until a fresh command arrives.
- **Holds.** Every armed cycle `write()` sends one frame with every declared
  input. An unclaimed input, or a claimed one whose command is NaN (the
  ros2_control default before a controller writes) or infinite, carries the
  hold: 0 for `velocity` and `duty`, and for `position` the last position
  sent, which starts at the measured position when the claim begins or ends.
  A new claim starts unset, so a value a previous controller left in the
  interface is never sent as a command. A position hold is always taken from
  a readable measured position (the switch fails without one), and a mode
  change while armed first drops the frames queued before the new holds, so
  the controller's first value in its new mode is never an old one.
- **The watchdog.** The plugin sends no keepalives: only `write()` feeds the
  device watchdog (`system.watchdog_ms`, 200 ms by default). If the
  controller_manager's loop stops, the device disarms within that time and
  `read()` then fails. The update rate must stay well above
  `1000 / watchdog_ms` Hz.
- **Link cost.** One frame of 4 + 4·n bytes (n declared command inputs of the
  loop) plus framing per `write()` while armed; at 100 Hz and six inputs
  about 4 KB/s of the 92 KB/s link.
- **Threads.** `prepare_command_mode_switch` does the mode write and ARM (it
  blocks, bounded by `timeout_ms`); `perform_command_mode_switch` only applies
  the claims. A claim prepared on the real-time thread (ros2_control's
  fallback controllers) is refused, because it would block the loop; so is
  `is_async` (the real-time thread is the one running `read()`). A stop-only
  prepare only validates, and perform applies stops from its own list. A
  prepared claim that is not performed within `timeout_ms` (another component
  refused the switch, or it timed out), or a second claim prepared before it
  is, makes `read()` fail, since a joint's mode may then not match its
  running controller.

## Lifecycle

- **configure**: reads the token, connects to the agent (its Hello says
  `client_kind` Ros, so the GUI shows "ROS is driving"), loads the device
  model and resolves every label and signal. Safe mode, a pending reboot, an
  invalid layout, a missing or failed instance, a missing signal or one of the
  wrong kind or type fails with a message naming the joint, the interface and
  what was found. (Loading the component already refuses unknown parameters,
  `<gpio>` and a state interface whose `data_type` is not `double`.)
- **activate**: refuses while another client holds the lease (unless
  `take_lease` is `preempt`; the component stays inactive), takes the lease,
  and sets up one subscription group per loop that holds a mapped signal; it
  waits for the first sample of each. It also configures one command group
  per loop that holds a command input (disarmed; nothing is sent until a
  claim). State-only use never arms. A device
  reboot since configure, or any failure after the Take, disconnects and
  returns `ERROR` (the component ends unconfigured): closing the connection is
  the only certain end of a lease. The holder check and the Take are two
  requests, so a GUI Take landing between them is preempted; the plugin then
  sees its lease end and fails the activation.
- **read()** (real time: `noexcept`, no allocation, blocking lock or device call)
  copies the newest sample of each group into the state interfaces. It returns
  `ERROR` when the device disarms while armed (the watchdog, a fault, a loop
  stall, or a DISARM from another client; the log names the status stream's
  disarm reason), the status stream is silent for three status periods (at
  least 300 ms) while armed, a prepared switch was not performed in time, a mode write
  while armed got no answer, the lease is lost (another client took it), the agent reports
  the device offline or shuts down, the connection drops, the device reboots,
  agent events were dropped (the lease state is then unknown), or a group's
  newest sample is older than `stale_timeout_ms` (by the sample's device time
  mapped through the agent's clock model). ros2_control then calls `on_error`,
  which logs the reason and disconnects without a device call; the agent
  clears the plugin's groups and ends its lease. The component is
  unconfigured, and nothing comes back by itself. Configure and activate it
  again (`ros2 control set_hardware_component_state drivelink active`) once
  the cause is fixed; H4c adds the diagnostics node's `recover`. While the
  component is inactive, `read()` returns `OK` and a fault seen then makes the
  next activate fail.
- **deactivate**: disarms if armed, clears the command and subscription groups
  and releases the lease; if the DISARM, a clear or the release is not
  confirmed it disconnects (the agent then disarms and clears) and returns
  `ERROR`.

## Tests

`test_mapping` covers the parameters, the conventions and every refusal
(no ROS, no device). `test_integration` (built when `DRIVELINK_HW_INTEGRATION`
is set, as `tools/ros_check.sh` does) runs the plugin through
`hardware_interface::ResourceManager` against `drivelink-vdev` behind
`drivelink-agent`: configure and activate, values following the device, the
lease refusal and `preempt`, `ERROR` on a lost lease, a stopped agent, a
device reboot (vdev on a PTY) and stale data, a reboot between configure and
activate, deactivate and activate again, cleanup and configure again, and a
wrong label or token. The command path runs with a 100 Hz read/write loop
thread as controller_manager would: claims arming and moving the mock plant,
releases disarming, a mode change while armed, duty in `open_loop`, NaN holds,
refused switches (two interfaces on a joint, a refused or unanswered ARM, a
claim from the loop thread, an unprepared claim), `ERROR` on a GUI Take, an
injected fault, a GUI DISARM and a watchdog trip, an abandoned prepare, no jump
after a velocity → position switch, and deactivating while claimed.
`host/test/ros_state_check.py` is the robot's state check (wheels turned by
hand, the IMU tilted) and `host/test/ros_command_check.py` its command check
(wheels off the ground, `forward_command_controller` per wheel and mode); both
have a `--mock` mode against vdev.
