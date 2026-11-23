# drivelink_hardware

The plugin exports one ros2_control hardware component per DriveLink device. Use the bringup package for a complete robot configuration and source build. [Bringup README](../drivelink_bringup/README.md) · [Mapping implementation](src/mapping.cpp) · [SystemInterface implementation](src/system.cpp) · [Plugin rules](CLAUDE.md)

## URDF mapping

```xml
<ros2_control name="drivelink" type="system">
  <hardware>
    <plugin>drivelink_hardware/DriveLinkSystem</plugin>
    <param name="device">robot</param>
  </hardware>
  <joint name="left_wheel_joint">
    <param name="instance">left</param>
    <command_interface name="velocity"/>
    <state_interface name="position"/>
    <state_interface name="velocity"/>
  </joint>
</ros2_control>
```

### Hardware parameters

| Parameter | Default | Meaning |
|---|---|---|
| `device` | required | The agent's alias of the device, or its 12-hex device id |
| `agent` | `tcp://127.0.0.1:47620` | The agent's address; a host name is resolved |
| `token_file` | `/etc/drivelink/token` | First line of the agent token file. See the bringup installation procedure for service-user access. |
| `token` | empty | The token inline, for tests and vdev; wins over `token_file` |
| `take_lease` | `refuse` | `refuse` reports the current holder. `preempt` permits a lease takeover. |
| `state_rate_hz` | `100` | 1..10000 Hz: how often the plugin needs fresh state, normally the controller_manager's `update_rate`. Each loop's group runs at its loop rate divided by `max(1, floor(loop rate / state_rate_hz))`, so it is never slower than this and spends no more of the link than needed |
| `stale_timeout_ms` | `auto` | `read()` returns `ERROR` when a group's newest sample is older than this. `auto`: ten group periods, at least 100 ms; `0` turns the check off |
| `timeout_ms` | `3000` | Each agent and device request while configuring and activating, each stage of a command mode switch, and how long a prepared switch may wait for its perform |

Unknown parameters fail validation.

### Joints and sensors

Each joint or sensor names a configured, running device instance by its `instance` label. State interfaces use `double`. Override the default signal for one interface with `<param name="state.INTERFACE">SIGNAL</param>`. The plugin rejects `<gpio>` entries.

| Component | Interface | Signal |
|---|---|---|
| joint | `position`, `velocity`, `duty`, any other name | the signal of the same name |
| sensor | `orientation.w/x/y/z` | `quaternion.w/x/y/z` |
| sensor | `angular_velocity.x/y/z` | `gyro.x/y/z` (rad/s) |
| sensor | `linear_acceleration.x/y/z` | `accel.x/y/z` (m/s², gravity included, as `sensor_msgs/Imu` expects) |
| sensor | any other name | the signal of the same name |

### Command interfaces

Joints can declare the following `double` interfaces. Each maps to a distinct device `f32` input. Controllers can claim one interface per joint at a time, with different joints allowed to use different modes. A switch must release the previous claim on a joint before claiming its replacement.

| Interface | Input | Mode entry |
|---|---|---|
| `position` | `position_sp` (rad) | `position` |
| `velocity` | `velocity_sp` (rad/s) | `velocity` |
| `duty` | `duty_sp` (-1..1, open loop) | `open_loop` |

Override input and mode names with `command.INTERFACE` and `mode.INTERFACE`. The mode parameter defaults to `mode` and can be renamed with `mode_param`. Use `none` to skip mode writes. A controller without a mode parameter also skips those writes. Duty is normalized open-loop output.

A position claim or release needs a readable measured position for its initial hold. The plugin subscribes to that measurement even when the joint exports no position state interface. It uses `state.position` when supplied, otherwise the `position` signal. A new claim starts unset, so an old controller's retained command cannot drive the joint. While holding, position uses the last position sent and velocity and duty use zero. Configure the controller update rate comfortably above `1000 / watchdog_ms` Hz.

## Lifecycle diagnosis

| Phase | What to inspect |
|---|---|
| Configure | Check token access, agent reachability and device discovery. Safe mode, an invalid layout, pending reboot and missing or failed instances prevent configuration. Mapping errors name the affected joint or interface and report the signal or type found. |
| Activate | Check the reported lease holder, boot identity and first sample for every mapped loop. State-only activation leaves the device disarmed. A failure after lease acquisition disconnects. |
| Command switch | Check the mode write, measured position and ARM result/detail. An unanswered ARM triggers DISARM. Claiming prepare must run off the real-time thread, which rules out claims from fallback controllers on that thread. The plugin also rejects `is_async`. A prepared claim that expires, or overlaps another claim, makes `read()` fail. |
| Active read | Check the disarm reason, loss of lease or connection, reboot, observer event loss and mapped sample age. Status silence while armed is detected after three status periods with a 300 ms minimum. An unanswered mode write while armed also fails the active component. |
| Inactive read | Returns `OK`. A fault observed while inactive can prevent the next activation. |
| Deactivate | Check confirmation of DISARM, group clears and lease release. An uncertain cleanup disconnects and returns `ERROR`. |

After an active error, correct the logged cause and explicitly restore the hardware and controllers. [Recovery procedure](https://taruntom1.github.io/drivelink_v2/ros/recover/) · [Real-time and error-path rules](CLAUDE.md)

## Verification

| Scope | Entry point |
|---|---|
| Pure mapping and configuration validation | [test_mapping.cpp](test/test_mapping.cpp) |
| Plugin lifecycle and command switching through the agent | [test_integration.cpp](test/test_integration.cpp), enabled by `DRIVELINK_HW_INTEGRATION`. Run `python tools/validate.py --only ros` or `--only ros-jazzy` from the repository root. |
| Physical state and command checks | [State check](../../test/ros_state_check.py) and [command check](../../test/ros_command_check.py). Use the H4a and H4b commands, equipment and pass criteria. Both scripts support `--mock` for vdev. [Hardware checklist](../../../plans/host-status.md#deferred-hardware-checks) |

See the development guide for ROS prerequisites and container setup. [Validation matrix](../../../docs/development.md#validation-toolsvalidatepy)
