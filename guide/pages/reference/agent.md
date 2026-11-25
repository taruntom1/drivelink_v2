# Agent configuration, service, and network

Use this reference after [installing the agent]({{base}}pi/agent/) to look up a setting, network requirement, or service behaviour.

## Configuration

The packaged agent reads `/etc/drivelink/agent.json` at startup. Edit it while the robot can safely stop and use the restart in [agent setup]({{base}}pi/agent/).

| Setting | Default or allowed value | Meaning |
| --- | --- | --- |
| `bind` | `127.0.0.1` | Address the agent listens on. Use the Pi’s LAN address to share it with your PC. |
| `port` | `47620` | TCP port clients use to connect. |
| `token_file` | `token` | Path to the token. A relative path is taken from the configuration file’s directory. The agent reads the first line. |
| `token` | 8–128 printable ASCII bytes | Inline alternative to `token_file`. Keep the token private. |
| `devices` | Empty list, up to 8 devices | Each entry names an `alias`, `endpoint`, and optional `baud`. The list has to fit the agent’s 1,024-byte device reply. |
| `devices[].alias` | 1–32 characters | Start with a lowercase letter, then use lowercase letters, digits, underscores, or hyphens. A 12-digit hexadecimal string is reserved for a device ID. |
| `devices[].endpoint` | `serial:///dev/ttyAMA0` for the reference Pi | Path to the protocol port. A virtual device can use `tcp://IP:PORT`. |
| `devices[].baud` | `"auto"` (or `9600`–`4000000`) | Serial link speed. `"auto"` detects the board’s rate, remembers it across reconnects, and confirms serial link trials when the same device ID reconnects. |
| `name` | Host name | Name shown in discovery. Use 1–63 bytes of UTF-8 with no period. |
| `advertise` | `true` | Advertise non-loopback addresses using multicast DNS (mDNS), a local discovery protocol. |
| `hello_timeout_ms` | `5000` | Time allowed for a client’s first request. |
| `retry_ms` | `2000` | Delay before reopening an offline device. |
| `request_timeout_ms` | `3000` | Time allowed for a device request. |
| `history_size` | `256`, range 16–4096 | Number of recent entries retained per device. |

## Service and ports

| Item | What to check |
| --- | --- |
| Service | `drivelink-agent` runs as the `drivelink` system user, which is in `dialout` for access to serial ports. The service logs to the system journal. |
| Token | `/etc/drivelink/token` is readable by root and by members of the `drivelink` group. |
| TCP 47620 | Carries device lists and control traffic. The default loopback bind is reachable from the Pi only. |
| UDP 5353 | Carries discovery for `_drivelink._tcp.local.` on the local network. Type the address manually if discovery is unavailable. |
| Network | Connections use the token without encryption. Use a trusted LAN. |

Stopping or restarting the service disarms each online device. See [upgrade or remove the agent]({{base}}pi/maintenance/) for how package updates affect the service.
