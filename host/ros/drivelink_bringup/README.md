# drivelink_bringup

An example two-wheel diff-drive robot with an IMU for ROS 2 Lyrical and Jazzy.
DriveLink connects to the device through `drivelink-agent`. The URDF maps
the two velocity-controlled joints and the IMU to device instance labels.
The same wheel separation in the controller YAML sets the URDF joint origins.

Install the release from apt using the [ROS 2 installation guide](https://taruntom1.github.io/drivelink_v2/ros/install/).

## Build from source

From a colcon workspace with this repository at `src/drivelink_v2` and ROS
installed, run:

```sh
sudo sh src/drivelink_v2/tools/setup_linux.sh ros
source /opt/ros/$ROS_DISTRO/setup.bash
colcon build --packages-up-to drivelink_bringup
source install/setup.bash
```

## Launch and recovery

- [Start the two-wheel example](https://taruntom1.github.io/drivelink_v2/ros/example/)
- [Launch arguments and hardware interfaces](https://taruntom1.github.io/drivelink_v2/reference/ros/)
- [Diagnostics and recovery](https://taruntom1.github.io/drivelink_v2/ros/recover/)

The packaged launch configures the hardware and starts the state
broadcasters. The drive controller starts inactive. The diagnostics node
connects separately as an observer. Its recovery service acknowledges
the fault and restores the hardware and the controllers specified in
`recovery_controllers`. See the [plugin contracts](../drivelink_hardware/README.md)
for lifecycle and interface behavior.

## Checks

`python tools/validate.py --only ros` runs the recovery tests and the installed
example launch against vdev behind the agent. `ros-jazzy` runs the same checks
in the Jazzy container. The mock and real-robot driving checks share
[`host/test/ros_bringup_check.py`](../../test/ros_bringup_check.py). The real
run uses an interactive terminal and the first-run safety procedure.
`ros-deb-install` runs the mock check from apt-installed packages in clean
containers.
