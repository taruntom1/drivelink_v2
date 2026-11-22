# Diagnose and recover the ROS drive

You need the DriveLink ROS launch and a terminal with the ROS environment loaded. You’ll identify why the drive stopped, acknowledge a cleared fault, and choose when to restore control.

1. Read `/diagnostics` and check the drive controller and hardware state. Look for device online state, lease holder, arm state, active errors, and the last disarm reason or diagnostic record. The GUI also shows [faults]({{base}}concepts/safety/).

   ~~~sh
   ros2 topic echo /diagnostics diagnostic_msgs/msg/DiagnosticArray --once
   ros2 control list_hardware_components
   ros2 control list_controllers
   ~~~

   After a hardware error, look for the `drivelink` component in `unconfigured` and the drive controller in `inactive`. If the hardware is active and you stopped the drive normally, use the [drive activation step]({{base}}ros/drive/#check-motion-on-stands) when ready.

2. Stop teleop, fix the underlying condition, and make sure the board is disarmed. To clear a device fault while keeping the drive stopped, call `ack_fault`. The service should report `success=True` when the condition has cleared and the device accepts the acknowledgement.

   ~~~sh
   ros2 service call /drivelink_diagnostics/ack_fault std_srvs/srv/Trigger '{}'
   ~~~

> **Safety.** Recovery can arm the device. Lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. Stop all motion publishers and release any other client’s lease before recovering.

3. When the hardware is unconfigured after an error and you’re ready to restore control, call `recover`. It acknowledges the fault and restores the hardware and the configured controllers. By default this includes the drive controller, which arms the board. Check the response for `success=True`, then list hardware and controllers again. Confirm the active controllers and the arm state before sending a fresh command.

   ~~~sh
   ros2 service call /drivelink_diagnostics/recover std_srvs/srv/Trigger '{}'
   ~~~

4. If recovery times out or reports incomplete cleanup, verify the arm state and list controllers again. Deactivate any controller still claiming a motion interface before trying again. Use the GUI to verify that the other lease holder has released control. See [ROS hardware errors]({{base}}troubleshooting/#ros-hardware-is-in-error).

When you are finished, deactivate the drive controller and check that the board is disarmed.

~~~sh
ros2 control switch_controllers --deactivate diff_drive_controller --strict
~~~
