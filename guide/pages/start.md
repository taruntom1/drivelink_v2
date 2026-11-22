# Start here

DriveLink runs motors and reads sensors on an ESP32-S3 board. You use the desktop app to configure the board, watch its measurements, and try changes on the bench. Start with a computer and a USB data cable to see how everything fits together and choose what to set up.

## How the pieces fit

<figure>

![On the bench, the PC app talks to the ESP over USB. On a robot, the Pi agent talks to the ESP over UART. The PC app reaches the agent over the network and ROS 2 can use the agent on the Pi.]({{base}}assets/drive-link-paths.svg)

</figure>

- **At your desk**, the [desktop app]({{base}}pc/install/) talks directly to the ESP over USB. The [firmware]({{base}}concepts/firmware/) on the ESP runs the control loops and reads the sensors.
- **On a robot**, an [agent]({{base}}concepts/agent/) on a Raspberry Pi talks to the ESP over UART, the board's serial connection. The desktop app talks to the agent over your local network.
- **With [ROS 2]({{base}}concepts/ros/)**, robot software talks to the ESP through that same agent. The ESP still runs the motor loops.

## Choose your starting point

Start with the ESP and USB data cable. You can install the app, flash the board, and explore the **Device** screen with the ESP on its own. Add a motor and encoder once you can connect. The [reference build]({{base}}reference/bench/) lists the parts and explains which alternatives fit. The Pi and IMU are optional for a first motor test.

> **Safety.** Before a step that can move a motor, lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. Disconnect power before changing wiring.

- [Install the app and start the PC bench walkthrough]({{base}}pc/install/).
- [Prepare the Pi and connect over the LAN]({{base}}pi/prepare/).
- [Install ROS 2 and start the robot example]({{base}}ros/install/).
