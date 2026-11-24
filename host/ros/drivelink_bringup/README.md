# drivelink_bringup

Example two-wheel differential drive with an IMU. Released packages, launch arguments and operator recovery are documented in the user guide. [Installation](https://taruntom1.github.io/drivelink_v2/ros/install/) · [Example launch](https://taruntom1.github.io/drivelink_v2/ros/example/) · [Launch reference](https://taruntom1.github.io/drivelink_v2/reference/ros/) · [Recovery](https://taruntom1.github.io/drivelink_v2/ros/recover/)

## Source map

| Concern | Source and contract |
|---|---|
| Geometry | The launch reads `diff_drive_controller.ros__parameters.wheel_separation` from the selected controller YAML, checks that it is positive and finite, and passes it to Xacro for the wheel joint origins. [Controller defaults](config/controllers.yaml) · [Launch](launch/example.launch.py) · [Robot Xacro](urdf/example.urdf.xacro) |
| Instance mapping | Xacro passes the left, right and IMU labels into the hardware configuration. [Hardware Xacro](urdf/drivelink.ros2_control.xacro) · [Plugin mapping](../drivelink_hardware/README.md) |
| Startup | The launch starts the joint-state and IMU broadcasters first. Their spawner must succeed before the drive controller loads inactive. [Launch](launch/example.launch.py) |
| Recovery | Diagnostics observes through a separate connection. The recovery service acknowledges the fault and restores hardware and the controllers named in `recovery_controllers`. [Diagnostics node](src/diagnostics.cpp) · [Recovery logic](src/recovery.hpp) |

## Build from source

Install the ROS build prerequisites, then run the following from the colcon workspace root. [ROS validation and prerequisites](../../../docs/development.md#validation-toolsvalidatepy)

```sh
source /opt/ros/${ROS_DISTRO}/setup.bash
colcon build --packages-up-to drivelink_bringup
source install/setup.bash
```

## Verification

| Coverage | Entry point |
|---|---|
| Recovery logic and installed launch against vdev behind the agent | Run `python tools/validate.py --only ros` from the repository root. Use `ros-jazzy` for the same check in the Jazzy container. [Recovery tests](test/test_recovery.cpp) |
| Apt-installed launch | Select `ros-deb-install` and its required package inputs. The check uses signed test packages in clean containers. [Validation matrix](../../../docs/development.md#validation-toolsvalidatepy) |
| Physical bringup | Run the H4c procedure from an interactive terminal with the specified equipment and operator confirmation. The same script supports a vdev dry run with `--mock`. [Hardware checklist](../../../plans/host-status.md#deferred-hardware-checks) · [Bringup check](../../test/ros_bringup_check.py) |
