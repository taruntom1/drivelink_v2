# Install from apt

Use Ubuntu 24.04 or 26.04 on a 64-bit Intel, AMD or ARM computer. You need an internet connection, curl, GnuPG (the tool that checks the signing key) and permission to use sudo. After setup, Ubuntu’s package manager, apt, can install and update DriveLink.

## Add the repository

1. Download the repository’s public key. You should see the key’s details, including its 40-character fingerprint.

   ~~~sh
   sudo install -d -m 0755 /etc/apt/keyrings
   sudo curl -fsSLo /etc/apt/keyrings/drivelink-archive-keyring.asc '{{apt_url}}/drivelink-archive-keyring.asc'
   gpg --show-keys --fingerprint /etc/apt/keyrings/drivelink-archive-keyring.asc
   ~~~

2. Compare that fingerprint with the one below. Ignore spaces. If it differs, stop here. {{fingerprint_source}}

   ~~~text
   {{fingerprint}}
   ~~~

3. Add the package source and refresh apt. You should see apt read package information from DriveLink without a signature error.

   ~~~sh
   . /etc/os-release
   printf 'Types: deb\nURIs: {{apt_url}}/\nSuites: %s\nComponents: main\nSigned-By: /etc/apt/keyrings/drivelink-archive-keyring.asc\n' \
     "$VERSION_CODENAME" | sudo tee /etc/apt/sources.list.d/drivelink.sources
   sudo apt update
   ~~~

## Install a package

| What you need | Ubuntu | Package |
| --- | --- | --- |
| Desktop app | 26.04 | drivelink |
| Agent, the background service that connects to the board | 24.04 or 26.04 | drivelink-agent |
| ROS 2 bringup, the files that start the example robot | 26.04 (Lyrical) | ros-lyrical-drivelink-bringup |
| ROS 2 bringup | 24.04 (Jazzy) | ros-jazzy-drivelink-bringup |

For the ROS 2 packages, set up the [official ROS 2 apt repository for Lyrical](https://docs.ros.org/en/lyrical/Get-Started/Installation/Ubuntu-Install-Debs.html) or [Jazzy](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html) first. The bringup package pulls in its other DriveLink ROS packages.

Replace PACKAGE with the name from the table. Apt should list the package and its dependencies and report that installation finished.

~~~sh
sudo apt install PACKAGE
~~~

For the desktop app, search for **DriveLink** in your application launcher. For the agent or ROS 2 packages, verify that apt lists the package and its installed version.

~~~sh
apt list --installed PACKAGE
~~~

When you update the agent, the running service keeps its current version until it restarts. Choose a time when the robot can safely stop.
