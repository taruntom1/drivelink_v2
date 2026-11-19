# H3a-2 implementation plan (approved 2026-11-19)

The approved plan for H3a-2, kept for reference. The design record is
[host-architecture.md](../host-architecture.md), the contract
[docs/protocol/agent.md](../../docs/protocol/agent.md) (agent protocol v2),
the state [host-status.md](../host-status.md).

## User decisions (2026-11-19)

- **Clock:** clients ping the agent (TimePing) and compose that fit with the
  agent's device-to-agent ClockModel event, so mapped time holds on remote
  clients as well as on the agent's host (chosen over a loopback-only model
  and over wall-clock time).
- **Exclusive operations** are the brief's list plus REBOOT and
  EXIT_SAFE_MODE. ACK_FAULT, diag CLEAR, every param SET (live or staged) and
  all reads stay open.
- **Every lease end** (release, Take by another client, the holder
  disconnecting): the old holder's frames stop, fast-path DISARM, its command
  groups are cleared, then a waiting Take is granted. ARM waits while an
  earlier holder's command group remains on the device.
- **CLI:** a global `--lease` (the lease lasts for one command), the
  `agent-clients` command, and the holder in `agent-devices`.
- **Models:** Opus 5.5 (high) writes the contracts (`agent.md` v2, the
  registry, `agent.hpp`), the lease and group logic with their tests, the
  agent wiring and the Session clock and agent ops, integrates, fixes and
  reviews; Sonnet 5.5 subagents (high) build from the contracts: the codec and
  its tests, then the CLI changes and the integration scenarios. Independent
  review on Opus (high), as the brief requires.

## Scope

- Agent protocol v2: the device record gains the holder; LeaseTake,
  LeaseRelease, ClientList, TimePing ops; Lease, ClockModel, History events;
  `LeaseChange` and `HistoryKind` in the registry.
- Agent: the lease state machine, command-group virtualization, the routing
  of ARM, command groups, command frames and exclusive operations, the arm
  state from the status stream, history (`history_size`), ClockModel events,
  TimePing answers, nonzero unique client ids.
- libdrivelink: the codec, `composeClock`, agent pings and the composed clock
  in `Session`, `Session::agentRequest`, `agentLeaseTake`,
  `agentLeaseRelease`, `agentClients`.
- CLI: `--lease`, `agent-clients`, the holder in `agent-devices`.

## Tests and checks

- Unit: the codec (golden bytes, round trips, refusals, history chunking,
  clock composition), the lease state machine, the op classification, command
  slots and LIST, history and the status parse, `history_size`.
- Integration (vdev): arming through the lease, a preempting Take, the
  holder's disconnect and release, exclusive operations through the CLI, the
  client list, history and the clock on connect, a lease per device, lobby
  refusals; the H3a-1 scenarios moved to v2.
- Hardware: vdev only (the brief). The firmware changes only by the
  descriptor hash constant, so nothing is flashed.
