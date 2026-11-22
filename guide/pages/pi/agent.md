# Install the agent and add the device

You need a Pi running Ubuntu, a [wired board]({{base}}pi/wire/), and the [DriveLink apt source]({{base}}apt/#add-the-repository). Keep motor power off. You’ll run the [agent]({{base}}concepts/agent/) as a service and register the board under the alias `base`, the name you’ll use to choose it.

1. Install the agent and its companion package. The service should be active. The installation creates `/etc/drivelink/agent.json` with an empty device list and `/etc/drivelink/token` with a random access token.

   ~~~sh
   sudo apt install drivelink-agent drivelink-cli
   systemctl is-active drivelink-agent
   ~~~

2. List the ports. For the reference Pi wiring, look for `/dev/ttyAMA0`. A USB-to-UART bridge can appear as `/dev/drivelink/` followed by its serial number. The ESP board’s native USB port is for the firmware console.

   ~~~sh
   drivelink-agent --list-ports
   ~~~

3. Open the agent configuration. For the reference UART, set the contents to the example below. If you use the USB-to-UART bridge, change the endpoint to the bridge’s `serial://` entry printed by the previous command. Check that the device has the alias `base` and baud rate `921600`.

   ~~~sh
   sudoedit /etc/drivelink/agent.json
   ~~~

   ~~~json
   {
     "bind": "127.0.0.1",
     "port": 47620,
     "token_file": "token",
     "devices": [
       {"alias": "base", "endpoint": "serial:///dev/ttyAMA0", "baud": 921600}
     ]
   }
   ~~~

4. Restart the service while the robot is safe to stop. The status should return `active`. Check its journal for errors if it fails to start. Restarting disarms devices connected through the agent.

   ~~~sh
   sudo systemctl restart drivelink-agent
   systemctl is-active drivelink-agent
   sudo journalctl -u drivelink-agent -n 30 --no-pager
   ~~~

The default address allows connections from the Pi itself. To reach it from the desktop app, [connect over the LAN]({{base}}pi/connect/). To run a local ROS 2 client, [grant its user token access]({{base}}ros/install/#grant-token-access).
