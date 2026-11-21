# Raspberry Pi setup (drivelink-agent)

How to put `drivelink-agent` on a Raspberry Pi 5 that drives a DriveLink
device (an ESP32-S3) over its GPIO UART. The agent owns the serial port and
shares the device with the GUI, the CLI and ROS 2 on the Pi or over the LAN
([agent/README.md](../host/agent/README.md)).

> **Not yet checked on a Pi.** The packages, the service and the udev rule
> are tested in containers and in WSL with systemd (H3c). The Pi-specific
> steps below (the UART overlay, the console, the wiring) are unverified
> until the H3c row in [host-status.md](../plans/host-status.md), "Deferred
> hardware checks", passes. The apt repository (step 4) is tested in
> containers (H3d) and published from v0.1.0 on.

## 1. Ubuntu on the Pi

Install **Ubuntu Server 26.04 LTS (64-bit)** with Raspberry Pi Imager. Ubuntu
24.04 works too, without the GUI package. Boot, log in, and update:

```sh
sudo apt update && sudo apt full-upgrade
```

## 2. The GPIO UART

The ESP's protocol link is UART0 at 921600 baud. On the Pi, use UART0 on
GPIO 14/15 (header pins 8 and 10).

1. Turn the UART on and keep the console off it. In
   `/boot/firmware/config.txt`, add under `[all]`:

   ```
   dtparam=uart0=on
   ```

   In `/boot/firmware/cmdline.txt` (one line), delete any
   `console=serial0,115200` or `console=ttyAMA0,115200`. Keep
   `console=tty1`.
2. Stop a login prompt from taking the port:

   ```sh
   sudo systemctl disable --now serial-getty@ttyAMA0.service
   ```

3. Reboot. `ls -l /dev/serial0 /dev/ttyAMA*` shows the ports. On a Pi 5,
   `ttyAMA0` is GPIO 14/15 and `ttyAMA10` is the separate debug header.
   `drivelink-agent --list-ports` (after step 4) labels them.

## 3. Wiring

3.3 V logic on both sides. Never connect 5 V to an ESP pin.

| Pi 5 (header pin) | ESP32-S3 |
|---|---|
| GPIO 14, TXD (8) | GPIO 44, U0RXD |
| GPIO 15, RXD (10) | GPIO 43, U0TXD |
| GND (6) | GND |

On a dev board, the CH343 adapter also drives GPIO 44. Disconnect the CH343
(or its TX line) before connecting the Pi's TX. Otherwise the two transmitters
fight. A board on USB instead of the GPIO UART works too (step 5).

## 4. Install the packages

From DriveLink's signed apt repository (H3d): add its key and source once,
then install with apt. `apt upgrade` then brings new releases.

```sh
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/drivelink-archive-keyring.asc \
  https://taruntom1.github.io/drivelink_v2/drivelink-archive-keyring.asc
gpg --show-keys /etc/apt/keyrings/drivelink-archive-keyring.asc
. /etc/os-release
printf 'Types: deb\nURIs: https://taruntom1.github.io/drivelink_v2/\nSuites: %s\nComponents: main\nSigned-By: /etc/apt/keyrings/drivelink-archive-keyring.asc\n' \
  "$VERSION_CODENAME" | sudo tee /etc/apt/sources.list.d/drivelink.sources
sudo apt update
sudo apt install drivelink-agent drivelink-cli
```

The fingerprint `gpg --show-keys` prints must equal
[packaging/apt/FINGERPRINT](../packaging/apt/FINGERPRINT) in the DriveLink
repository. The repository holds the latest release for Ubuntu 24.04 and
26.04 ([packaging/apt/README.md](../packaging/apt/README.md)).

Without the repository, download the two arm64 packages for your Ubuntu from
a release (`drivelink-cli_<version>~ubuntu26.04_arm64.deb` and
`drivelink-agent_<version>~ubuntu26.04_arm64.deb`) and install them:

```sh
sudo apt install ./drivelink-cli_*_arm64.deb ./drivelink-agent_*_arm64.deb
```

From source, build the packages on the Pi and install what it makes:

```sh
sudo sh tools/setup_linux.sh agent      # host instead, for the GUI package on 26.04
python3 tools/package_linux.py
sudo apt install ./build/deb/drivelink-cli_*.deb ./build/deb/drivelink-agent_*.deb
```

The install creates:
- the `drivelink` system user, in `dialout`;
- a random token in `/etc/drivelink/token`, readable by root and the
  `drivelink` group;
- `/etc/drivelink/agent.json`, which lists no devices yet.

It then starts the `drivelink-agent` service on `127.0.0.1:47620`. Check it:

```sh
systemctl status drivelink-agent
drivelink-cli --endpoint tcp://127.0.0.1:47620 --token "$(sudo cat /etc/drivelink/token)" agent-devices
```

## 5. Add the device

```sh
drivelink-agent --list-ports
```

It prints one line per port it finds, without opening any port:
- `/dev/serial0` and the `ttyAMA` UARTs;
- `/dev/drivelink/<USB serial>` for a board on USB. The udev rule names CH343
  adapters and ESP32-S3 native USB this way. The native USB port is the
  firmware's console, not its protocol port.

Paste the line you want into `"devices"` in `/etc/drivelink/agent.json`
(`sudo nano /etc/drivelink/agent.json`), for example:

```json
{
  "bind": "127.0.0.1",
  "port": 47620,
  "token_file": "token",
  "devices": [
    {"alias": "base", "endpoint": "serial:///dev/ttyAMA0", "baud": 921600}
  ]
}
```

Rename the alias as you like. Then restart the agent and check the device:

```sh
sudo systemctl restart drivelink-agent
drivelink-cli --endpoint tcp://127.0.0.1:47620 --token "$(sudo cat /etc/drivelink/token)" agent-devices
drivelink-cli --endpoint tcp://127.0.0.1:47620 --token "$(sudo cat /etc/drivelink/token)" --device base info
```

A restart, a stop or a reboot of the Pi DISARMs every device.

## 6. The GUI on another machine (LAN)

By default only programs on the Pi can reach the agent. To use the GUI from
your PC, set `"bind"` to the Pi's LAN address, or to `"0.0.0.0"` for every
interface, and restart. The agent then advertises itself over mDNS:
- on the PC, the GUI's Agent tab lists it;
- `drivelink-cli agent-browse` lists it from any machine.

Give the GUI the token (`sudo cat /etc/drivelink/token`). There is no TLS:
use a trusted network only. UDP 5353 (mDNS) and TCP 47620 must be open.

## 7. ROS 2 bringup on the Pi

Use ROS 2 Lyrical on Ubuntu 26.04. With this repository at
`~/ros_ws/src/drivelink_v2` and the agent set up above, run
`sudo usermod -aG drivelink "$USER"` to grant token access. Log in again
before building and launching the workspace.

```sh
cd ~/ros_ws
sudo sh src/drivelink_v2/tools/setup_linux.sh ros
source /opt/ros/lyrical/setup.bash
colcon build --packages-up-to drivelink_bringup
source install/setup.bash
ros2 launch drivelink_bringup example.launch.py device:=robot controllers_file:=/path/to/robot.yaml
```

Substitute the agent alias and a controller YAML with the robot's measured
wheel geometry. The launch starts disarmed. Configure device labels and
follow the first-run and recovery procedures before activating the drive.
The [bringup guide](../host/ros/drivelink_bringup/README.md) documents the
ROS topics and services.

## Upgrades and removal

- **Upgrade:** `sudo apt update && sudo apt upgrade` from the repository, or
  install newer release files the same way. An upgrade never
  restarts the running agent, because a restart DISARMs every device.
  `apt` prints a reminder. Run `sudo systemctl restart drivelink-agent`
  when the robot is safe to stop. Your `agent.json` is kept; apt asks before
  replacing an edited one.
- **Remove:** `sudo apt remove drivelink-agent` stops the agent and keeps
  `/etc/drivelink`.
- **Purge:** `sudo apt purge drivelink-agent` deletes the config, the token
  and the `drivelink` user.
- **Logs:** `journalctl -u drivelink-agent`.
