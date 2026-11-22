# Troubleshooting and FAQ

Start with what you see in the app, agent, or ROS terminal. Keep motor power off while checking wiring or a fault. After a fix, verify the device state before allowing motion.

## No serial port or a protocol mismatch

- **No serial port.** Try a known data cable in the board’s USB-to-UART connector. The native USB connector carries the firmware console. Close anything already using the port. In Ubuntu, check your serial permissions in [Connect and tour]({{base}}pc/connect/). On the Pi, see the [UART checks](#the-pi-uart-does-not-work).
- **Protocol mismatch.** Compare the app, firmware, and agent releases. Use a matched set from [Downloads]({{base}}downloads/). When the GUI can still connect for an update, follow [Update firmware]({{base}}pc/update/). For a first flash or incompatible firmware, follow [Flash in the browser]({{base}}flash/) and read the erase warning.

## Arm is refused

> **Safety.** Keep the wheels off the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch before retrying ARM.

Use the app’s status and diagnostics or the ROS log to identify the refusal. See the [status and diagnostic codes]({{base}}reference/codes/) for a numeric code.

| What you see | What to check |
| --- | --- |
| Device metadata or status is still loading | Wait for **Device** to finish loading. If status stays unavailable, reconnect and check the serial or agent link. |
| Fault latched (`FaultLatched`) | Fix the cause, confirm that no active fault condition remains, then acknowledge it. |
| Stop still in progress (`DisarmRaced`) | Wait for the previous disarm to settle. Confirm the device is safely disarmed before a deliberate new attempt. |
| Device is starting or stopping (`NotAtBoot`) | Wait for the app to reconnect and the running loops to appear in **Device**. |
| Safe mode (`SafeMode`) | Check the boot diagnostics and saved layout. Follow [Arming and faults]({{base}}concepts/safety/) to correct the cause before leaving safe mode. |
| Invalid layout (`LayoutInvalid`) | Fix the reported layout problem in **Layout**. Review the loop, pins, and connections before saving and rebooting. |
| Reboot required (`RebootPending`) | Review pending edits, save them, and reboot while disarmed. Reconnect and confirm the active layout. |
| Firmware update active (`OtaActive`) | Finish or cancel the update using the app. Wait until the update reports its outcome. |
| Firmware update awaits confirmation (`OtaPendingConfirm`) | Check the [firmware update]({{base}}pc/update/) status and resolve the update before trying to arm. |
| Instance failed to start (`StartFailed`) | Find the failed instance in **Layout**. Check the device diagnostic, its configured pins, and its dependencies. |
| An active diagnostic blocks arming (`BlocksArm`) | Find the active diagnostic and fix its cause before acknowledging a fault or retrying. |
| No configured command group (`NoCommandGroup`) | Prepare an input in **Monitor → Manual command**, or check the expected ROS hardware and controllers. |
| Agent denies control (`LeaseRequired`) | Check **Clients and lease**. Coordinate the handoff and use [Take lease]({{base}}pi/control/) when safe. |
| Agent is clearing an earlier lease’s commands (`Busy` after a handoff) | Confirm the new holder and wait for cleanup to finish before retrying. |
| The GUI says another owner armed the device | Coordinate with the owner, stop motion, and disarm before trying to arm from this window. |
| Command keepalives are still pending or blocked | Wait for the app to finish setting up its command groups. If the check stays blocked, read the detail on **Device** and close any unused client holding group slots. |

## A wheel or encoder runs backwards

Disarm first. If a positive command turns the wheel backwards, check the motor mapping and the actuator’s `invert` parameter. If the wheel turns forward but its position or velocity is negative, check the encoder’s `invert` parameter. If feedback stays at zero, check the encoder’s power, signals, and joint mapping. Repeat the safe [first-spin check]({{base}}pc/first-spin/) after each change.

## A fault or watchdog stop appears

Open **Device** and note the diagnostic source and code. Look it up in [Diagnostic codes]({{base}}reference/codes/#diagnostic-codes). Investigate the cause before acknowledging the fault. A watchdog stop means fresh commands stopped arriving. Check the command publisher, the lease, and the serial or network connection before another run. In ROS 2, use the [recovery procedure]({{base}}ros/recover/).

## The Pi UART does not work

- Compare the port listing with [Pi setup]({{base}}pi/prepare/). Choose `/dev/ttyAMA0` for GPIO 14 and 15. The Pi 5’s `/dev/ttyAMA10` is the separate debug connector.
- Check that the serial login service and console leave the UART free. In `/etc/drivelink/agent.json`, check the selected endpoint and the 921,600 baud rate.
- With power off, check that TX goes to RX in both directions and the boards share ground. Check that the Pi and the USB bridge cannot drive the ESP’s RX pin together. See [Pi wiring]({{base}}pi/wire/). After restarting the agent, recheck the device list and service journal.

## Agent discovery and connection

| Symptom | What to do |
| --- | --- |
| Agent missing from **Found on the network** | Check that both computers are on the same trusted LAN. Enable **Find agents on the network** in **Settings**, then use **Search again**. Check that the agent listens on the LAN and mDNS on UDP 5353 is available. |
| Agent is unreachable | Type its address in the **Agent** tab. Check the bind address, TCP port 47620, and whether a firewall or network isolation prevents the connection. |
| Token refused | Copy the current token from the Pi and retry **List devices**. Replace any stale remembered token. |
| Agent is incompatible | Update the app or the agent to a matching release. |
| Device still offline | Verify the selected alias, [UART or USB endpoint]({{base}}pi/agent/), and service logs. |
| A lease is held by another client | Watch as an observer or ask that client to [release control]({{base}}pi/control/). A takeover stops the current holder and disarms the device. |

## ROS hardware is in ERROR

Check `/diagnostics` and the controller manager log for the event that stopped the component. A disarm during motion, stale measurements, loss of the lease, a device reboot, or a lost agent connection can stop it. Fix the cause and [recover deliberately]({{base}}ros/recover/). An observer can still read the device while the drive is stopped.

## ROS topics are missing or silent

If the launch is running but a terminal sees no messages, check that both terminals loaded the same ROS environment and use the same `ROS_DOMAIN_ID`. DDS is the middleware that discovers and delivers ROS messages. Check that both processes permit discovery on the intended network. When all ROS processes run on the same WSL machine with Fast DDS, you can use shared memory. Close the running ROS processes and set the option in each terminal before relaunching.

~~~sh
export FASTDDS_BUILTIN_TRANSPORTS=SHM
~~~

Run the [topic checks]({{base}}ros/example/#start-and-verify) again. Joint state and IMU should each produce a message.
