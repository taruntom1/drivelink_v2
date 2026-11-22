# ROS 2 in five minutes

If you’re deciding where DriveLink fits in a robot, this page explains the roles of ROS 2, the agent, and the ESP.

**ROS 2** gives programs on a robot a common way to exchange messages. Each program, or **node**, can publish values to a named **topic** or subscribe to them. For example, a controller can read wheel positions and produce new wheel-speed commands.

**ros2_control** is a ROS 2 framework for connecting controllers to hardware. The DriveLink hardware plugin turns the controller’s commands into requests to the [agent]({{base}}concepts/agent/) and turns board measurements into wheel or IMU state. The ESP does the fast motor control on the board.

**Bringup** means starting the nodes, controllers, and hardware needed for a robot. The DriveLink bringup package includes an example for a robot that turns by changing the relative speed of its left and right wheels, called **differential drive**.

You can use the PC bench with the desktop app and an ESP over USB. For the ROS 2 pieces, see the [official ROS 2 concepts](https://docs.ros.org/en/lyrical/Concepts.html) and the [DriveLink package list]({{base}}apt/#install-a-package).
