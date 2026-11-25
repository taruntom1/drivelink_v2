# drivelink-agent

Linux service for sharing device links with authenticated TCP clients. Client-visible routing and lease semantics are defined in the protocol. The agent rules cover thread ownership, shutdown ordering and the emergency path. [Wire contract](../../docs/protocol/agent.md) · [Agent rules](CLAUDE.md)

## Source map

| Change | Owner |
|---|---|
| CLI flags, JSON settings and token loading | [Entry point](src/main.cpp), [configuration parser](src/config.cpp) |
| Device opening, request forwarding and connection shutdown | [Agent runtime](src/agent.cpp) |
| Access policy, virtual slots and LIST synthesis | [Group routing](src/groups.cpp) |
| Lease transitions | [Lease state machine](src/lease.cpp) |
| Recent device events and arm state | [History](src/history.cpp) |
| Local serial enumeration and network discovery | [Port discovery](src/ports.cpp), [mDNS responder](src/mdns.cpp) |
| Installed service, device rules and package lifecycle | [Deployment files](deploy/) |

## Development run

Build the `drivelink-agent` target with the headless Linux superbuild. Run it in the foreground against a local vdev with a development token file:

```sh
drivelink-agent --token-file ./agent-token --device test=tcp://127.0.0.1:8765
```

`READY tcp://127.0.0.1:47620` indicates that the listener is up. Device opens happen in the background and failures retry at the configured interval. Use the CLI's device list to confirm a device is online. Logs go to stderr. SIGINT or SIGTERM closes client sessions and requests DISARM on online devices before exit. [Host build](../README.md#build) · [CLI reference](../cli/README.md) · [Virtual device](../vdev/README.md)

## Configuration and diagnosis

`--config PATH` loads JSON. CLI flags override file settings. Use the shipped config as a fixture and the parser for accepted keys, validation and precedence. A relative `token_file` resolves from the configuration file's directory and uses its first line. An empty device list supports lobby queries. The agent rejects a configured list that cannot fit one ListDevices response. Serial devices default to `"baud": "auto"` (also emitted by `--list-ports`), or accept a fixed rate (`9600`–`4000000`). On reconnect to the same `device_id`, the agent keeps the detected rate as its hint and confirms an active serial link trial automatically; a first connect to an unknown device leaves any active trial for a client (`LINK_CONFIRM`, exclusive). [Shipped config](deploy/agent.json) · [Configuration parser](src/config.cpp)

For an installed service, check `journalctl -u drivelink-agent` for config and device-open failures. Exit status 2 indicates a config error and suppresses automatic restart. A LAN listener can still serve TCP if binding the mDNS socket fails. Inspect discovery and connectivity independently. [systemd unit](deploy/drivelink-agent.service) · [Operator installation](https://taruntom1.github.io/drivelink_v2/pi/agent/) · [Configuration reference](https://taruntom1.github.io/drivelink_v2/reference/agent/)

## Verification

- Run `python tools/validate.py --only agent` from the repository root for Linux unit and vdev integration tests. On Windows this check uses WSL. Both tests carry the `agent` CTest label. [Test registration](CMakeLists.txt) · [Integration harness](test/agent_integration.py)
- Use the package checks to exercise staged and installed service behavior. The bench deployment check requires systemd and a device. [Validation matrix](../../docs/development.md#validation-toolsvalidatepy) · [Bench procedure](../../docs/bench.md)
