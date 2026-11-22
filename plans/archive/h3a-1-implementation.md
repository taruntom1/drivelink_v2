# H3a-1 implementation plan (approved 2026-11-19)

The approved plan for H3a-1, kept for reference. The design record is
[host-architecture.md](../host-architecture.md), the contract
[docs/protocol/agent.md](../../docs/protocol/agent.md), the state
[host-status.md](../host-status.md).

## User decisions (2026-11-19)

- **Split** H3a into H3a-1 (server, devices, access, routing, events, agent
  MSG_TYPEs, library client, agent check and CI) and H3a-2 (lease, exclusive
  operations, clock model, history, client list, CLI lease commands).
- **Lease Take preempts** (H3a-2): the agent sends fast-path DISARM, revokes
  the old holder, then grants.
- **Agent refusals use new shared Detail values** (`AgentVersion`, `BadToken`,
  `UnknownDevice`, `DeviceOffline`, `LeaseRequired`, 54..58). The firmware
  never sends them, so core stays v10; only the generated hash constant
  changes.
- **Interim rule until H3a-2:** ARM, CMD_CONFIG, CMD_CLEAR answered
  `Busy/LeaseRequired`, command-group frames dropped, DISARM forwarded from
  every client.
- **Models:** Opus 5.5 (high) plans, builds the contracts, the Session
  changes, the agent core and the group map, integrates and reviews; Sonnet 5.5
  subagents (high) build from written contracts: A the agent codec and its
  tests, B the agent config and its tests, C the tooling (`agent` check,
  `setup_linux.sh agent`, CI job, development.md rows), D the CLI options and
  the integration script, and E (after review) the extra integration
  scenarios. Independent review on Opus (high), as the brief requires.

## Scope

- `host/agent/` (`drivelink-agent`, Linux only, `COLCON_IGNORE`): JSON config
  and flags, per-device Sessions retried every 2 s, TCP on `127.0.0.1:47620`
  by default, the Hello (agent version, core version, token, device) refused
  before any device frame, the lobby, ListDevices, the Closing event.
- Routing: requests forwarded through the device's Session (seq rewriting),
  replies sent back with the client's seq; status, core and diag events to
  every client; per-client virtual subscription groups (id and channel
  rewriting, epoch filtering, LIST synthesis, cleanup on disconnect); time
  pings and command frames dropped.
- Device open: GET_INFO, DISARM, clear every group, then accept clients.
- libdrivelink: the agent codec (`agent.hpp`), `SessionOptions::agent`,
  `listAgentDevices`, `retire_quarantine`, `observe_streams`.
- CLI: `--token`, `--device`, `--client-name`, `agent-devices`.
- Registry: `agent_command` 0x30, `agent_response` 0x70, `agent_event` 0xF0
  with generator directions `host_to_agent` / `agent_to_host`.
- Tooling: `linux-no-gui` superbuild with the agent, `validate.py agent`
  (through WSL on Windows), `setup_linux.sh agent`, CI job `agent`.

## Tests and checks

- Unit: codec (round trips, golden bytes, bounds, truncation), config and
  flags, routing policy, group map, LIST synthesis, the Session quarantine and
  DISARM after ID exhaustion.
- Integration: the agent with vdevs, the CLI and raw clients.
- Hardware: vdev only (the brief); the bench board through WSL is H3b. The
  firmware changes only by the descriptor hash constant, so nothing is
  flashed.
