# Upgrade or remove the agent

You need the Pi and access to its terminal. Stop the robot before restarting or removing the agent. You’ll either start the upgraded service or remove it and decide what to keep.

## Upgrade the service

1. Refresh apt and upgrade. If the agent gets an update, apt prints a reminder to restart it. The running service continues with its current version until restart.

   ~~~sh
   sudo apt update && sudo apt upgrade
   ~~~

2. When it’s safe to stop the robot, restart the service. Expect `active`, then reconnect in the app and check the device is disarmed.

   ~~~sh
   sudo systemctl restart drivelink-agent
   systemctl is-active drivelink-agent
   ~~~

## Remove the service

Stop using the device from the desktop app or ROS 2. Remove the package to stop the service and keep `/etc/drivelink`, including the device list and token.

~~~sh
sudo apt remove drivelink-agent
~~~

Apt should report that it removed the package. Install it again to reuse the saved settings.

> **Warning.** Purging deletes the device configuration and token. Any client that saved the old token will need the new one after a fresh install.

To delete the service configuration and its system account as well, use purge. Apt should report that it purged the configuration files.

~~~sh
sudo apt purge drivelink-agent
~~~
