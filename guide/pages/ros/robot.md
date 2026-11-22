# Describe your own robot

You need a working [two-wheel example]({{base}}ros/example/), the ROS environment loaded, and the labels of your wheel controllers and IMU. You’ll copy the installed robot description and see how its names connect ROS to DriveLink.

1. Copy the installed robot description and the DriveLink mapping into your own directory. URDF describes the robot’s links and joints. Xacro adds reusable pieces and values to the URDF. You should see both files in `robot-description`.

   ~~~sh
   mkdir -p "$HOME/robot-description"
   cp "$(ros2 pkg prefix --share drivelink_bringup)"/urdf/*.xacro "$HOME/robot-description/"
   ls "$HOME/robot-description"
   ~~~

2. Open `example.urdf.xacro`. Check the joint names `left_wheel_joint` and `right_wheel_joint` against `left_wheel_names` and `right_wheel_names` in your controller YAML. If you rename a joint in your description, update the YAML and the matching joint name in the `drivelink_diff_drive` call. The names should agree in all three places.
3. In `drivelink.ros2_control.xacro`, follow each joint’s `instance` parameter to the device controller label. For the reference layout the labels are `left_drive` and `right_drive`. The sensor’s `instance` parameter uses the IMU’s label. Keep the IMU frame in the YAML aligned with its link in the URDF. See the [hardware interfaces]({{base}}reference/ros/#hardware-interfaces) for units and optional overrides.
4. Expand your edited xacro and check that it produces a URDF. Open the generated file and verify the `ros2_control` section contains your joint names and device labels.

   ~~~sh
   xacro "$HOME/robot-description/example.urdf.xacro" device:=base left_label:=left_drive right_label:=right_drive imu_label:=imu > "$HOME/robot-description/robot.urdf"
   ~~~

Use the generated description in your own robot launch with `robot_state_publisher` and `ros2_control_node`. The [installed example launch]({{base}}reference/ros/#bringup-launch-arguments) uses its bundled description. It accepts your labels and controller YAML for changes that fit the example’s geometry. With your own launch, check the joint and IMU topics disarmed and follow the [stands test]({{base}}ros/drive/#check-motion-on-stands) before motion.
