# Agent protocol v1 (H3a-1)

`drivelink-agent` owns the serial ports of one or more devices on a Linux host
(the robot's Pi, or WSL on the PC) and lets several clients share each device
over TCP. This file is the contract between the agent and its clients
(`libdrivelink`'s agent client, the CLI, later the GUI and the ROS plugin).
The design and its reasons are in
[host-architecture.md](../../plans/host-architecture.md), "`drivelink-agent`".

H3a-1 builds the connection, access, routing and subscription groups. The
control lease, command groups for the lease holder, exclusive operations, the
clock model, history and the client list come in H3a-2, which bumps this
protocol to v2. Until then **no client can arm or command a device through the
agent** (see "Interim rule").

## Connection

- TCP, one connection per device. The agent listens on `127.0.0.1:47620` by
  default; listening on another address is opt-in in its config.
- Every frame uses the device framing ([framing.md](framing.md)), with the
  agent's own `MAX_PAYLOAD` of 1024.
- The client's first frame must be an `agent_command` Hello (below). Any other
  first frame, a malformed frame header, or no Hello within the hello timeout
  (5 s) closes the connection without a reply. Nothing from a device reaches a
  client before its Hello is accepted.
- After an accepted Hello the connection is either a **lobby** (the Hello named
  no device: only `agent_command` is allowed, any other frame closes it) or a
  **device connection** (it carries that device's frames, rewritten as
  "Routing" says).
- The agent closes a device connection when the device goes offline or the
  agent shuts down, after sending a Closing event when it can.

## Agent messages

All three MSG_TYPEs are agent-only ([registry.md](registry.md)); the agent
never forwards them to a device. Version byte `1` is the agent protocol
version, independent of core (10), time (1) and diag (3). Strings are
`length u8 | bytes` (no terminator); integers are little-endian.

| MSG_TYPE | Name | Payload |
|---|---|---|
| 0x30 | `agent_command` | `version u8 \| seq u8 \| op u8 \| body` |
| 0x70 | `agent_response` | `version u8 \| seq u8 \| op u8 \| result u8 \| detail u8 \| body` |
| 0xF0 | `agent_event` | `version u8 \| event u8 \| body` |

`result` and `detail` use the core `Result` and `Detail` enums. A response
echoes the command's `seq` and `op`. Refused responses have an empty body,
except the version refusal below. Ops are `AgentOp` (Hello 1, ListDevices 2);
events are `AgentEvent` (Closing 1). An unknown op is answered
`InvalidArgument/Unsupported`; a body of the wrong length
`InvalidArgument/None`.

### Hello (op 1)

Request body: `core_version u8 | token str8 | device str8 | client_name str8`.

- `core_version` is the core protocol version the client speaks.
- `token` is 1..128 bytes, compared in constant time with the agent's token.
- `device` is empty (lobby), a device alias, or a device id as 12 lowercase hex
  digits (the GET_INFO id). An alias never looks like a device id.
- `client_name` is 0..32 bytes chosen by the client (for example
  `drivelink-cli`), shown to other clients from H3a-2.

The agent checks, in this order, and answers the first failure:

| Check | Result / detail | Body |
|---|---|---|
| The command's `version` byte is 1 and `core_version` is the agent's core version | `InvalidArgument/AgentVersion` | `agent_version u8 \| core_version u8` (what the agent speaks) |
| The token matches | `InvalidArgument/BadToken` | empty |
| The agent has room for another client (64) | `NoMemory/Capacity` | empty |
| The device names a configured alias or a known device id | `NotFound/UnknownDevice` | empty |
| That device is online | `Busy/DeviceOffline` | empty |

On a version refusal the response's own version byte is still 1, so any client
can decode it. After any refusal the agent closes the connection once the
response is sent. A second Hello on an accepted connection is answered
`InvalidArgument/WrongState` and changes nothing.

Accepted body: `client_id u16 | kind u8 | record`, where `kind` is 0 for a
lobby and 1 for a device connection; `record` is present only for a device
connection and is a device record (below). `client_id` identifies the
connection in the agent's log (16 bits, it wraps after 65535 connections).

### ListDevices (op 2)

Empty body, allowed on any accepted connection. Answer:
`count u8 | count * record`, in config order.

Device record: `alias str8 | online u8 | device_id[6] | name str8 |
endpoint str8 | clients u8`.

- `online` is 1 once the agent has opened the port and read GET_INFO, until the
  link fails.
- `device_id` and `name` are the last ones read from the device (zero bytes and
  empty before it was ever online). The agent rereads them on connect, on a
  device Boot, and after an accepted SET_NAME, REVERT or FACTORY_RESET.
- `endpoint` is the device's configured endpoint (`serial:///dev/...`, or
  `tcp://...` for vdev).
- `clients` counts the device connections to it.

### Closing event (1)

Body: `reason u8` (`AgentCloseReason`: DeviceOffline 1, Shutdown 2). Sent to a
connection just before the agent closes it.

## Routing on a device connection

Frames from the client:

| Frame | What the agent does |
|---|---|
| `core_command` 0x01 | Checks the core header (a wrong version is answered as a device would: family and op 0xFF, `InvalidArgument/None`). Then the interim rule, then the group rules below; everything else is forwarded on the device's link with a new `seq`, and the reply is sent back with the client's `seq`. |
| `disarm` 0x03 | Always forwarded at once on the fast path; the reply goes back with the client's `seq`. |
| `diag_query` 0x23 | Forwarded with a new `seq`, the reply sent back with the client's. |
| `time_request` 0x02 | Dropped and counted. The agent alone pings the device; it sends clients its clock model in H3a-2. |
| command group 0xA0–0xBF | Dropped and counted (interim rule). |
| `agent_command` 0x30 | ListDevices; Hello is refused as above. |
| anything else | Dropped and counted. |

When the device does not answer, the client gets no reply either and its own
timeout applies; a mutation stays uncertain, as on a direct link. When the agent's queue toward the device is full, a request is answered
`Busy/None` (a diag query: result Busy) without reaching the device.

Frames from the device:

| Frame | Goes to |
|---|---|
| `status_stream` 0x80, `diag_event` 0xC0, `core_event` 0xC1 | every device connection |
| subscription groups 0x81–0x9F | the client that owns the group, on its own channel (below), if the epoch is current |
| replies 0x41, 0x63 | the client whose request it answers |
| `time_response` 0x42 | nobody |

### Interim rule (H3a-1)

`ARM` (system op 2), `CMD_CONFIG` and `CMD_CLEAR` are answered
`Busy/LeaseRequired` without reaching the device, and command-group frames are
dropped. System DISARM (op 3) and the fast path 0x03 are forwarded from every
client at once. H3a-2 replaces this with the lease.

### Subscription groups

Group ids are device resources; the agent gives each client its own view.

- A client sees subscription ids 0..S-1 and command ids 0..C-1, where S and C
  are the device's pool sizes (`limits.sub_groups`, `limits.cmd_groups`).
- `SUB_CONFIG` on a client id with no device slot takes the lowest device slot
  no client owns; with none left it is answered `InvalidArgument/Capacity` (the
  same answer as an id out of range). A refused CONFIG on a newly taken slot
  gives the slot back. The agent rewrites the group id in requests and the
  channel in replies, so the client sees `channel = 0x81 + its id`.
- `SUB_ENABLE` on a client id with no slot is answered
  `InvalidArgument/UnknownRef`, as for an unconfigured slot. `SUB_CLEAR` on one
  is answered Ok with the last epoch that client saw for that id (0 if none),
  as clearing an empty slot is on a device. An accepted CLEAR frees the slot.
- `LIST` answers with the client's own view: its slots with their device
  configuration (group id rewritten), every other id empty (flags 0, loop 0xFF,
  divider 1, n 0, the last epoch the client saw). Pages follow the device rule
  in [core.md](core.md), "Group".
- A stream frame from a device slot goes only to its owner, and only when its
  epoch equals the epoch of the owner's last accepted mutation of that slot.
  Other frames are dropped and counted.
- When a client disconnects, the agent clears its slots on the device. A slot
  whose CLEAR fails is not handed out again until a later CLEAR succeeds.
- On a device Boot every slot is free again (the device lost its groups);
  clients learn of the Boot from the diag Boot event, as on a direct link.

## Device lifecycle

- The agent opens each configured device and retries a failed or lost one every
  2 s, without affecting the others.
- On every open, before it accepts clients for that device, the agent reads
  GET_INFO (a core version other than its own keeps the device offline, with a
  log line), sends DISARM, and clears every configured subscription and command
  group, so nothing left by an earlier owner keeps streaming or commanding.
- When the link fails, the device goes offline: its connections get a Closing
  event (DeviceOffline) and are closed; the lobby shows it offline.

## Versions

| Date | Change | Agent protocol after it |
|---|---|---|
| 2026-11-19 (H3a-1) | First version: Hello, ListDevices, Closing; routing and subscription groups; the interim rule | v1 |
