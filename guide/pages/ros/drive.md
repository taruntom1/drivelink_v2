# Drive with the keyboard

You need the [example running disarmed]({{base}}ros/example/), keyboard teleop installed, and wheels that report forward rotation with the correct sign. You’ll check motion on stands first, then repeat it briefly on a clear floor.

> **Safety.** Lift and secure the wheels clear of the ground, use a current-limited supply, set a duty cap of 0.2 or lower at both the actuator and controller, and keep one hand on the power switch. Stop if the direction, speed, or response is unexpected.

## Check motion on stands

1. With ROS loaded, start keyboard teleop at a low linear and angular speed. The terminal should print its key map. Keep focus on this terminal when driving. The example expects `TwistStamped`, a velocity message with a time stamp.

   ~~~sh
   ros2 run teleop_twist_keyboard teleop_twist_keyboard --ros-args -p stamped:=true -p speed:=0.05 -p turn:=0.2
   ~~~

   See the [keyboard teleop key map](https://github.com/ros2/teleop_twist_keyboard) for the movement and speed keys.

2. Confirm the safety setup before switching on motor power. From another terminal with ROS loaded, activate the drive controller. It should report a successful switch and the board should show **Armed**. The wheels should stay still until there is a fresh command.

   ~~~sh
   ros2 control switch_controllers --activate diff_drive_controller --strict
   ~~~

3. Briefly press **i** in the teleop terminal. Both wheels should turn forward. Press **k** to send a stop. The controller also commands zero once `/cmd_vel` goes quiet for 0.25 seconds. If a wheel runs backwards or fails to stop, cut motor power and [check polarity and faults]({{base}}troubleshooting/).
4. Deactivate the drive controller and check that the board is disarmed. Exit teleop with Ctrl+C and switch off motor power.

   ~~~sh
   ros2 control switch_controllers --deactivate diff_drive_controller --strict
   ~~~

## Move to the floor

> **Safety.** Complete the stands check with wheels off the ground, a current-limited supply, a low duty cap, and a hand on the power switch. Disarm and switch off motor power before lowering the robot. Clear the area and keep the power switch accessible for the floor test.

1. Lower the disarmed robot onto a level, clear floor. Repeat the teleop start and activation commands above. Briefly drive forward at the same low speed, then press **k**. The robot should move forward and stop. If it does not, cut motor power.
2. Check estimated travel on `/odom`. The position should change in the direction the robot moved. Compare it with measured travel and [check the wheel dimensions]({{base}}ros/example/#set-the-wheel-dimensions) if they differ substantially.

   ~~~sh
   ros2 topic echo /odom nav_msgs/msg/Odometry --once
   ~~~

3. Run the deactivation command again, then exit teleop and the launch with Ctrl+C. Check the device is disarmed and switch off motor power.
