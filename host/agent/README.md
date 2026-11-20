# drivelink-agent

`drivelink-agent` owns the serial ports of one or more DriveLink devices on a
Linux host (the robot's Pi, or WSL on the PC) and shares each device with
several clients over TCP: the CLI, the GUI's remote mode (H3b-1) and the ROS
plugin (H4a). Linux only; not a colcon package (`COLCON_IGNORE`).

The wire contract is [docs/protocol/agent.md](../../docs/protocol/agent.md)
(agent protocol v3: v2 plus the client kind, H3b-1). The design and its reasons are in
[host-architecture.md](../../plans/host-architecture.md), "`drivelink-agent`".

Per device, one client at a time holds the **control lease**: only it may ARM,
configure command groups and send command frames, and the exclusive operations
(SAVE, REVERT, FACTORY_RESET, REBOOT, EXIT_SAFE_MODE, SET_NAME, layout,
capture, testgen, actions, OTA) need it, or a disarmed device that nobody
holds. Take preempts. When a lease ends (release, a Take, the holder
disconnecting) the agent disarms the device and clears the old holder's
command groups. DISARM is forwarded from every client. Reads, param writes,
ACK_FAULT and subscriptions stay open to all.

## Build

The superbuild builds it on Linux (`DRIVELINK_BUILD_AGENT`, default on off
Windows); `linux-no-gui` builds it without Qt:

```sh
cmake --preset linux-no-gui -S host/superbuild
cmake --build host/superbuild/build/linux-no-gui --target drivelink-agent
```

On Windows, `python tools/validate.py --only agent` builds and tests it in WSL
([docs/development.md](../../docs/development.md)).

## Install

The `drivelink-agent` package (H3c) is the way to run it on a robot. It is
built for Ubuntu 26.04 and 24.04, amd64 and arm64, from `VERSION` with the
release in the package version (`<version>~ubuntu26.04`). Get it from a
release or from `python3 tools/package_linux.py` (`build/deb/`). It
Recommends `drivelink-cli`. A Raspberry Pi walk-through, from the UART to the
LAN bind, is [docs/pi-setup.md](../../docs/pi-setup.md).

```sh
sudo apt install ./drivelink-cli_<version>_<arch>.deb ./drivelink-agent_<version>_<arch>.deb
```

- **What it installs:**
  - `/usr/bin/drivelink-agent`;
  - the `drivelink-agent` systemd unit;
  - `/usr/lib/udev/rules.d/70-drivelink.rules`;
  - `/etc/drivelink/agent.json`, a conffile: `bind` `127.0.0.1`,
    `token_file` `token`, no devices.

  The files are in [deploy/](deploy/). A first install creates the
  `drivelink` system user (in `dialout`) and a random token in
  `/etc/drivelink/token` (0640 `root:drivelink`), then enables and starts the
  service. With no devices it answers device lists only and logs
  `no devices configured`.
- **Adding a device:** `drivelink-agent --list-ports` prints a ready-to-paste
  `{"alias", "endpoint", "baud"}` line for each ESP port it finds:
  - `/dev/drivelink/<USB serial>`: the udev rule's names for a CH343 adapter
    or an ESP32-S3's native USB, which is the firmware's console, not its
    protocol port;
  - `/dev/serial0` and `/dev/ttyAMA*`.

  It never opens a port, since opening one can reset an ESP. Paste the line
  into `devices`, then run `sudo systemctl restart drivelink-agent`. The rule
  also tells ModemManager to leave these ports alone.
- **Upgrade never restarts the agent** (safety, user decision 2026-11-20).
  Stopping it DISARMs every device, so `apt upgrade` leaves the old process
  serving and prints that `sudo systemctl restart drivelink-agent` loads the
  new version. Only a first install, or a reinstall after remove, starts the
  service.
- **Remove and purge:** remove stops and disables the service and keeps
  `/etc/drivelink`. Purge also deletes the config, the token and the
  `drivelink` user.
- **The unit:**
  - runs as `drivelink` with `--config /etc/drivelink/agent.json`;
  - stops with SIGTERM (`TimeoutStopSec=30`, room to DISARM 8 devices);
  - restarts on failure, but not after exit 2, a config error: see
    `journalctl -u drivelink-agent`;
  - is hardened: no capabilities, a read-only system (`ProtectSystem=strict`),
    `@system-service` system calls, and the INET, UNIX and NETLINK address
    families.

  `/dev` is not restricted, because serial device numbers vary.

## Run

```sh
drivelink-agent --token-file /etc/drivelink/token \
  --device base=serial:///dev/ttyUSB0 --device arm=serial:///dev/ttyACM0@921600
```

It prints `READY tcp://127.0.0.1:47620` once it listens; devices open in the
background and are retried every 2 s. Logs go to stderr. SIGINT/SIGTERM send
each client a Closing event, DISARM every online device and exit.

Flags override the config file (`--config PATH`); `drivelink-agent --help`
lists them all. A config file:

```json
{
  "bind": "127.0.0.1",
  "port": 47620,
  "token_file": "token",
  "devices": [
    {"alias": "base", "endpoint": "serial:///dev/ttyUSB0", "baud": 921600},
    {"alias": "arm", "endpoint": "serial:///dev/ttyACM0"}
  ]
}
```

- `token` (8..128 printable ASCII bytes) or `token_file` (relative to the
  config file's directory; first line) is required. Keep the file private.
- `bind` other than loopback listens on the LAN (logged); every client still
  needs the token. There is no TLS: a trusted lab network is assumed.
- Aliases are 1..32 of `[a-z0-9_-]` starting with a letter and never 12 hex
  digits; endpoints are `serial://PATH` or `tcp://IP:PORT` (vdev). The device
  list must fit one ListDevices answer (1024 bytes); the agent refuses to start
  otherwise. The list may be empty (the packaged config): the agent then only
  answers device lists until a restart with devices.
- `hello_timeout_ms` (5000), `retry_ms` (2000) and `request_timeout_ms` (3000)
  tune the timings; `history_size` (256, 16..4096) is the history kept per
  device. At most 8 devices.
- On a LAN bind the agent advertises itself over mDNS/DNS-SD as
  `_drivelink._tcp` so the GUI's Agent tab and `drivelink-cli agent-browse`
  find it ([agent.md](../../docs/protocol/agent.md), "Discovery"). `name`
  (`--name`) is the instance name shown there (1..63 bytes of UTF-8, no `.`;
  default: the host name); `advertise: false` (`--no-advertise`) turns it off.
  A loopback bind never advertises. It needs UDP port 5353, which it shares
  with avahi or any other responder; without it the agent logs why and serves
  TCP as usual.

Through the agent the CLI adds `--device NAME --token TOKEN` (or
`DRIVELINK_AGENT_TOKEN`); `--lease` takes the lease for one command (it ends
when the CLI exits, which disarms the device); `agent-devices` lists the
devices and their lease holders and `agent-clients` a device's clients
([cli/README.md](../cli/README.md)).

## How it works

- One Asio thread owns the listener, every client socket, the routing state
  and each device's group map. Each device has its own libdrivelink `Session`
  (its own I/O thread). Session completions and observed frames are dispatched
  onto the agent thread, so the agent's state has a single owner.
- Opening a device runs on a helper thread with the synchronous API: open,
  GET_INFO (the core version must match), DISARM, clear every group. Only then
  do clients reach it. A lost link closes that device's clients and retries;
  the other devices are unaffected.
- Requests are forwarded through the device's `Session`, which assigns the
  device-side `seq`, times out and retires IDs; the agent answers each client
  with its own `seq`. Its Sessions use `retire_quarantine` (30 s or 10× the
  request timeout) because they never reconnect to reclaim IDs, and
  `observe_streams` to forward subscription frames.
- The pure logic has no I/O and is unit tested: `groups.cpp` (the routing
  policy, each device's virtual group map of subscription and command slots
  with allocation serials and Dirty slots, LIST synthesis), `lease.cpp` (free,
  held, ending with a waiting Take) and `history.cpp` (the bounded ring and
  the status-stream arm state).
- DISARM always goes out at once on the Session's fast path. Only when the
  Session cannot admit another (ordinary requests fill its queue and a DISARM
  is in flight) does one wait, at most one per connection; those waiting go
  out as one DISARM when the one in flight completes. The agent's own DISARM
  at a lease end uses the same path.
- A lease end: the old holder's frames stop at once; fast-path DISARM; when it
  completes (and any of the old holder's ARMs in flight completed, each
  followed by another DISARM) CMD_CLEAR of its command slots; then a waiting
  Take is granted. A failed CLEAR leaves the slot Dirty for the 2 s
  maintenance, and ARM is answered `Busy/None` until it is gone.
- mDNS (`mdns.cpp`, mjansson/mdns for sockets and record parsing): the
  responder runs on its own thread with fixed settings and shares nothing
  with the agent thread; it never touches a device, a client or the lease.
  One UDP socket per advertised interface (`IP_MULTICAST_ALL` off, so each
  sees only its interface's queries), interfaces re-scanned every 10 s,
  goodbyes at shutdown. The record sets and the answer rules are pure and
  unit tested.
- Clock: the device's Session pings it (periodic time); every 500 ms a
  changed model goes to the device's clients as a ClockModel event, and at
  once after a Boot. Clients' TimePing is answered with the agent's
  monotonic clock when the frame is read.

## Tests

- `agent_tests` (`agent-unit`): config and flags, `--list-ports` (aliases,
  lines, the scan over a temporary `/dev` and `/sys`), the routing policy (every
  core op open or exclusive), the group map of both kinds, LIST synthesis, the
  lease state machine, history and the status arm parse.
- `test/agent_integration.py` (`agent-integration`): the agent with vdevs and
  CLI or raw clients: access refusals, the same results as a direct vdev,
  refusals without the lease, DISARM from several clients, isolated groups and
  pool exhaustion, status to every client, device loss and return, Hello edge
  cases, a device reboot, Closing events, a slow client; arming through the
  lease, a Take that preempts an armed holder, a holder's disconnect and
  release, exclusive operations through the CLI, the client list, history and
  the clock model on connect, a lease per device and lobby refusals; an agent
  with no devices.
- The packages: the `deb` check's staged smoke (the shipped config, then a
  vdev device), CI's clean-container `host/test/installed_packages_smoke.py`
  (install, upgrade without a restart, remove, reinstall, purge, `systemd-analyze
  verify`, `udevadm verify`), and `host/test/deploy_check.py` on a host with
  systemd and a device ([docs/bench.md](../../docs/bench.md)).
- Both carry the ctest label `agent`; `validate.py agent` runs them and
  `host-linux` skips them.
