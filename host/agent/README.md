# drivelink-agent

`drivelink-agent` owns the serial ports of one or more DriveLink devices on a
Linux host (the robot's Pi, or WSL on the PC) and shares each device with
several clients over TCP: the CLI, the GUI's remote mode (H3b) and the ROS
plugin (H4a). Linux only; not a colcon package (`COLCON_IGNORE`).

The wire contract is [docs/protocol/agent.md](../../docs/protocol/agent.md)
(agent protocol v1). The design and its reasons are in
[host-architecture.md](../../plans/host-architecture.md), "`drivelink-agent`".

**H3a-1 state.** Access, devices, routing and subscription groups exist. The
control lease does not yet (H3a-2), so **no client can arm or command a device
through the agent**: ARM, CMD_CONFIG and CMD_CLEAR are refused with
`Busy/LeaseRequired` and command-group frames are dropped. DISARM is forwarded
from every client. Time pings from clients are dropped; the clock model comes
in H3a-2, so a client's mapped timestamps are not valid through the agent yet.

## Build

The superbuild builds it on Linux (`DRIVELINK_BUILD_AGENT`, default on off
Windows); `linux-no-gui` builds it without Qt:

```sh
cmake --preset linux-no-gui -S host/superbuild
cmake --build host/superbuild/build/linux-no-gui --target drivelink-agent
```

On Windows, `python tools/validate.py --only agent` builds and tests it in WSL
([docs/development.md](../../docs/development.md)).

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
  otherwise.
- `hello_timeout_ms` (5000), `retry_ms` (2000) and `request_timeout_ms` (3000)
  tune the timings.

Through the agent the CLI adds `--device NAME --token TOKEN` (or
`DRIVELINK_AGENT_TOKEN`); `agent-devices` lists the devices
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
- `groups.cpp` is the pure logic: the routing policy, each device's virtual
  subscription-group map (allocation serials, Dirty slots cleared after a
  client leaves) and LIST synthesis. It has no I/O and is unit tested.
- DISARM always uses the Session's fast path. One is on the link at a time;
  DISARMs that arrive meanwhile go out together right after it.

## Tests

- `agent_tests` (`agent-unit`): config and flags, the routing policy, the
  group map and LIST synthesis.
- `test/agent_integration.py` (`agent-integration`): the agent with vdevs and
  CLI or raw clients: access refusals, the same results as a direct vdev, the
  interim rule, DISARM from several clients, isolated groups and pool
  exhaustion, status to every client, device loss and return, Hello edge
  cases, a device reboot, Closing events and a slow client.
- Both carry the ctest label `agent`; `validate.py agent` runs them and
  `host-linux` skips them.
