# Share the device safely

You need a [remote GUI connection]({{base}}pi/connect/) and, to see a handoff, another connected client. You’ll learn to watch the device while someone else controls it and hand over the [control lease]({{base}}concepts/agent/) without arming unexpectedly.

1. Connect as an observer and open **Monitor**. You should see live measurements. Check the lease holder in the connection status before editing settings or trying to drive. An observer can read and disarm the device. A live parameter edit can affect a running robot, so coordinate it with the holder.
2. Ask the current holder to stop motion and release control. The agent disarms the device when the holder releases the lease or disconnects. Wait until the GUI shows the lease is free.

{{figure:remote-monitor-observer}}

> **Safety.** Before taking control for a motion test, lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. A takeover interrupts the current holder. Stop the robot and confirm the handoff with that operator.

3. In **Device → Clients and lease**, select **Take lease…** when you’re ready to be the holder. If another client still holds the lease, confirm that it is safe to stop the named client, then select **Take lease**. The app should say **Holds lease** for your client and the device should be disarmed. Follow [First spin]({{base}}pc/first-spin/) before sending a motion command.

{{figure:remote-take-ros}}

4. When finished, disarm and select **Release lease**. Check that control is available and the device is disarmed before another client takes over.

See [agent settings and network]({{base}}reference/agent/) for service and port details.
