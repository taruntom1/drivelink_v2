# Start the two-wheel example

You need the [ROS packages]({{base}}ros/install/), the agent device alias `base`, and two [wheel controllers]({{base}}pc/layout/) with the labels `left_drive` and `right_drive`. [Set up and calibrate the IMU]({{base}}pc/imu/) and note its label. The examples use `imu`. Replace any label that differs on your device. At the end you’ll see wheel and IMU measurements with the drive controller inactive.

## Set the wheel dimensions

Copy the installed controller settings into your home directory. Open the copied file and find `wheel_separation` and `wheel_radius` under `diff_drive_controller.ros__parameters`. Enter the distance between the centres of the wheel treads and one wheel’s radius, both in metres. The launch uses the same separation to position the joints in the robot description.

~~~sh
cp "$(ros2 pkg prefix --share drivelink_bringup)/config/controllers.yaml" "$HOME/robot-controllers.yaml"
~~~

## Start and verify

1. Put the robot on stands with the wheels clear of the ground. Keep motor power off. Release any control lease in the desktop app so ROS can acquire it. Start the example in a terminal with ROS loaded. Leave this terminal running.

   ~~~sh
   ros2 launch drivelink_bringup example.launch.py device:=base left_label:=left_drive right_label:=right_drive imu_label:=imu controllers_file:="$HOME/robot-controllers.yaml"
   ~~~

2. In a second terminal with ROS loaded, list the controllers. Expect `joint_state_broadcaster` and `imu_sensor_broadcaster` to be `active` and `diff_drive_controller` to be `inactive`. The board should show **Disarmed**.

   ~~~sh
   ros2 control list_controllers
   ~~~

3. Read one message from each [topic]({{base}}concepts/ros/). Turn each wheel forward by hand and re-read the joint states. Its position should increase in radians. Check that the IMU reports orientation, angular velocity, and acceleration. If a wheel reads backwards, [fix the polarity]({{base}}troubleshooting/#a-wheel-or-encoder-runs-backwards) before powering it.

   ~~~sh
   ros2 topic echo /joint_states sensor_msgs/msg/JointState --once
   ros2 topic echo /imu sensor_msgs/msg/Imu --once
   ~~~

Keep the example running for [keyboard driving]({{base}}ros/drive/). To stop here, press Ctrl+C in the launch terminal and check that the device is disarmed in the app.
