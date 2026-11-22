# Connect the desktop app over the LAN

You need the [agent running on the Pi]({{base}}pi/agent/) and the [desktop app]({{base}}pc/install/) on the same trusted local network (LAN). You’ll find the agent and open the device as an [observer]({{base}}concepts/agent/), ready to read its status.

1. Open `/etc/drivelink/agent.json` again. Set `bind` to the Pi’s LAN address, or use `0.0.0.0` to listen on all interfaces. Use `name` to give the agent a recognizable name such as `robot-pi`. Keep the Pi on a trusted network. Remote connections use a shared token and are unencrypted.
2. Restart the service while the robot is safe to stop. Check that it returns `active`. Display the [token]({{base}}concepts/agent/) in a private terminal to copy it to your desktop app. Keep it private.

   ~~~sh
   sudo systemctl restart drivelink-agent
   systemctl is-active drivelink-agent
   sudo cat /etc/drivelink/token
   ~~~

3. On the PC, open **Agent (remote)** on **Connect to a device**. Select the Pi under **Found on the network**. Its address should fill in. You can also type the Pi’s IP address into **Agent address**. The default TCP port is 47620.

{{figure:remote-agent-found}}

4. Enter the token and select **List devices**. Clear **Remember token** if the PC is shared. You should see `base` in the list. Choose the online device and select **Connect**. The **Device** page should open with device and connection status.

{{figure:remote-agent}}

If the list is empty, see [agent discovery and connection]({{base}}troubleshooting/#agent-discovery-and-connection). Once connected, [share control with other clients]({{base}}pi/control/).
