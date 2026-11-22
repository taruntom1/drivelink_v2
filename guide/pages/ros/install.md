# Install ROS 2 and DriveLink

You need an Ubuntu computer with the [DriveLink agent]({{base}}pi/agent/) connected to your device. Set up the [DriveLink apt source]({{base}}apt/#add-the-repository) on the same computer. At the end you’ll have ROS 2, the DriveLink bringup package that starts the example robot, and keyboard teleop, which sends drive commands.

## Install the packages

<div class="command-tabs" data-choice-group>
<div data-choice="Lyrical">

On Ubuntu 26.04, set up the [official Lyrical apt repository](https://docs.ros.org/en/lyrical/Get-Started/Installation/Ubuntu-Install-Debs.html). Install the packages and load the ROS environment in this terminal. The last command should print `/opt/ros/lyrical`.

~~~sh
sudo apt update
sudo apt install ros-lyrical-ros-base ros-lyrical-drivelink-bringup ros-lyrical-teleop-twist-keyboard
source /opt/ros/lyrical/setup.bash
ros2 pkg prefix drivelink_bringup
~~~

</div>
<div data-choice="Jazzy">

On Ubuntu 24.04, set up the [official Jazzy apt repository](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html). Install the packages and load the ROS environment in this terminal. The last command should print `/opt/ros/jazzy`.

~~~sh
sudo apt update
sudo apt install ros-jazzy-ros-base ros-jazzy-drivelink-bringup ros-jazzy-teleop-twist-keyboard
source /opt/ros/jazzy/setup.bash
ros2 pkg prefix drivelink_bringup
~~~

</div>
</div>

Load the matching ROS environment in each new terminal before running a `ros2` command.

## Grant token access

If you run ROS 2 on the agent’s computer, give your user access to the [agent token]({{base}}concepts/agent/). Sign out and back in after changing groups. The second command should show `drivelink` among your groups.

~~~sh
sudo usermod -aG drivelink "$USER"
~~~

~~~sh
id -nG
~~~

For a separate ROS computer, use a local user-readable copy of the token on a trusted LAN and supply its path as `token_file`. Keep token contents out of the published robot description.

Next, [start the example robot]({{base}}ros/example/).
