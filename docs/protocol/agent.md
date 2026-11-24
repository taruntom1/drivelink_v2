# Agent protocol v3 (H3a, H3b)

`drivelink-agent` owns the serial ports of one or more devices on a Linux host
(the robot's Pi, or WSL on the PC) and lets several clients share each device
over TCP. This file is the contract between the agent and its clients
(`libdrivelink`'s agent client, the CLI, the GUI and later the ROS plugin).
The design and its reasons are in
[host-architecture.md](../../plans/host-architecture.md), "`drivelink-agent`".

H3a-1 built the connection, access, routing and subscription groups (v1).
H3a-2 added the control lease, command groups for the lease holder, exclusive
operations, the clock model with agent time pings, history and the client
list (v2). H3b-1 added the client kind, so the GUI can tell ROS and other GUIs
apart (v3). H3b-2 added discovery over mDNS/DNS-SD ("Discovery" below), which
is outside the TCP protocol and left the version at 3.

## Connection

- TCP, one connection per device. The agent listens on `127.0.0.1:47620` by
  default; listening on another address is opt-in in its config.
- Every frame uses the device framing ([framing.md](framing.md)), with the
  agent's own `MAX_PAYLOAD` of 1024.
- The client's first frame must be an `agent_command` Hello (below). An
  `agent_command` with a version byte other than 3 is answered with the version
  refusal and closed; any other first frame closes the connection without a
  reply. Bytes that never form a valid frame are discarded, so a client that
  sends no Hello within the hello timeout (5 s) is closed. Nothing from a
  device reaches a client before its Hello is accepted.
- After an accepted Hello the connection is either a **lobby** (the Hello named
  no device: only `agent_command` is allowed, any other frame closes it) or a
  **device connection** (it carries that device's frames, rewritten as
  "Routing" says).
- On a device connection the agent sends, right after the Hello answer and
  before any device frame: the History event chunks (at least one), then one
  ClockModel event.
- The agent closes a device connection when the device goes offline or the
  agent shuts down, after sending a Closing event when it can (it waits at
  most 1 s for the event to drain).
- Limits: 64 accepted connections (128 including those before their Hello);
  32 forwarded requests in flight per connection (more are answered
  `Busy/None`, DISARM excepted); a connection that falls 4 MiB behind on its
  replies is closed, and stream frames for it are dropped beyond 256 KiB.

## Agent messages

All three MSG_TYPEs are agent-only ([registry.md](registry.md)); the agent
never forwards them to a device. Version byte `3` is the agent protocol
version, independent of core (10), time (1) and diag (3). Strings are
`length u8 | bytes` (no terminator); integers are little-endian; `f64` is an
IEEE 754 binary64, little-endian.

| MSG_TYPE | Name | Payload |
|---|---|---|
| 0x30 | `agent_command` | `version u8 \| seq u8 \| op u8 \| body` |
| 0x70 | `agent_response` | `version u8 \| seq u8 \| op u8 \| result u8 \| detail u8 \| body` |
| 0xF0 | `agent_event` | `version u8 \| event u8 \| body` |

`result` and `detail` use the core `Result` and `Detail` enums. A response
echoes the command's `seq` and `op`. Refused responses have an empty body,
except the version refusal below. An unknown op is answered
`InvalidArgument/Unsupported`; a body of the wrong length
`InvalidArgument/None`.

| Op (`AgentOp`) | Allowed on | Request body | Answer body |
|---|---|---|---|
| Hello 1 | first frame only | below | below |
| ListDevices 2 | lobby, device | empty | `count u8 \| count * record` |
| LeaseTake 3 | device | empty | empty |
| LeaseRelease 4 | device | empty | empty |
| ClientList 5 | device | empty | `count u8 \| count * client` |
| TimePing 6 | lobby, device | empty | `agent_us u64` |

An op on a connection it is not allowed on is answered
`InvalidArgument/WrongState`.

| Event (`AgentEvent`) | Body |
|---|---|
| Closing 1 | `reason u8` (`AgentCloseReason`: DeviceOffline 1, Shutdown 2) |
| Lease 2 | `holder_id u16 \| holder_name str8 \| holder_kind u8 \| previous_id u16 \| reason u8` |
| ClockModel 3 | the device's clock model (below), 69 bytes |
| History 4 | `last u8 \| count u8 \| count * entry` |

### Hello (op 1)

Request body: `core_version u8 | token str8 | device str8 | client_name str8 |
client_kind u8`.

- `core_version` is the core protocol version the client speaks.
- `token` is 1..128 bytes, compared in constant time with the agent's token.
- `device` is empty (lobby), a device alias, or a device id as 12 lowercase hex
  digits (the GET_INFO id). An alias never looks like a device id.
- `client_name` is 0..32 bytes chosen by the client (for example
  `drivelink-cli`), shown to other clients in ClientList and as the lease
  holder's name.
- `client_kind` is what the client is (`AgentClientKind`: Other 0, Gui 1,
  Cli 2, Ros 3), shown with its name. The CLI sends Cli, the GUI Gui and
  `drivelink_hardware` Ros; the GUI shows a Ros holder as "ROS is driving".
  It is a label for people, not an authorisation: the lease rules are the
  same for every kind. A larger value is answered `InvalidArgument/None`.

The agent checks, in this order, and answers the first failure:

| Check | Result / detail | Body |
|---|---|---|
| The command's `version` byte is 3 and `core_version` is the agent's core version | `InvalidArgument/AgentVersion` | `agent_version u8 \| core_version u8` (what the agent speaks) |
| The token matches | `InvalidArgument/BadToken` | empty |
| The agent has room for another client (64) | `NoMemory/Capacity` | empty |
| The device names a configured alias or a known device id | `NotFound/UnknownDevice` | empty |
| That device is online | `Busy/DeviceOffline` | empty |
| That device has fewer than 16 connections | `NoMemory/Capacity` | empty |

On a version refusal the response's own version byte is still 3. The refusal
body's layout is the same in every agent version, so a client of any version
can read what the agent speaks. After any refusal the agent closes the
connection once the response is sent. A second Hello on an accepted
connection is answered `InvalidArgument/WrongState` and changes nothing.

Accepted body: `client_id u16 | kind u8 | record`, where `kind` is 0 for a
lobby and 1 for a device connection; `record` is present only for a device
connection and is a device record (below). `client_id` identifies the
connection: it is never 0 and never the id of another open connection (ids
are reused after 65535 connections).

### ListDevices (op 2)

Answer: `count u8 | count * record`, in config order.

Device record: `alias str8 | online u8 | device_id[6] | name str8 |
endpoint str8 | clients u8 | holder_id u16 | holder_name str8 | holder_kind u8`.

- `online` is 1 once the agent has opened the port and read GET_INFO, until the
  link fails.
- `device_id` and `name` are the last ones read from the device (zero bytes and
  empty before it was ever online). The agent rereads them on connect, on a
  device Boot, and after an accepted SET_NAME, REVERT or FACTORY_RESET.
- `endpoint` is the device's configured endpoint (`serial:///dev/...`, or
  `tcp://...` for vdev).
- `clients` counts the device connections to it (at most 16).
- `holder_id` is the lease holder's `client_id`, 0 when nobody holds it, and
  `holder_name` its `client_name` (empty when nobody holds it), `holder_kind`
  its `client_kind` (0 when nobody holds it).

### ClientList (op 5)

Answer: `count u8 | count * client`, the device connections of this
connection's device in `client_id` order. Client: `client_id u16 | name str8 |
kind u8 | holder u8` (`kind` its `client_kind`; `holder` 1 for the lease
holder).

### TimePing (op 6)

Answer: `agent_us u64`, the agent's monotonic clock in microseconds when it
read the ping. A client fits its own clock to the agent's from these answers
(see "Time").

### Lease (op 3, op 4 and event 2)

One client of a device at a time holds its lease. Rules are under "The
lease".

- **LeaseTake (3)** preempts: if another client holds the lease, it is
  revoked and the agent disarms the device and clears the old holder's command
  groups first. The answer comes once the caller holds the lease: Ok. The
  holder taking again is answered Ok at once and changes nothing. A Take while
  a lease is ending (after a release or a disconnect) waits for that end and
  is then granted; a Take while another client's Take is already waiting is
  answered `Busy/None`. A handoff takes up to about three of the agent's
  request timeouts (DISARM, a repeated DISARM, the CLEARs; 9 s by default)
  when the device does not answer; libdrivelink waits 15 s. A client that
  gave up waiting may still be granted the lease later (the Lease event says
  so); closing the connection ends it.
- **LeaseRelease (4)** gives the lease up: Ok at once, then the agent disarms
  and clears as on a revocation. A client that does not hold it is answered
  `InvalidArgument/WrongState`.
- **Lease event (2)** goes to every connection of the device whenever the
  holder changes: `holder_id`, `holder_name` and `holder_kind` (0, empty
  and 0: nobody),
  `previous_id` (0: nobody) and `reason` (`LeaseChange`: Taken 1, Released 2,
  Disconnected 3). A client whose own id is `previous_id` has lost the lease.
  During a Take two events are sent when there was a holder: first the
  revocation (holder 0, reason Taken), then the grant (the new holder,
  previous 0, reason Taken).

### ClockModel (event 3)

The agent's fit of the device's clock to the agent's monotonic clock:
`flags u8 | device_reference_us u64 | host_reference_us u64 | span_us u64 |
age_us u64 | samples u32 | slope f64 | residual_us f64 | min_rtt_us f64 |
max_rtt_us f64` (69 bytes). Flags: bit 0 `offset_valid`, bit 1 `drift_valid`,
the others 0. Fields are those of libdrivelink's `ClockModel`
([time.md](time.md)); `host_reference_us` is in the agent's monotonic clock and
`age_us` is the model's age when the agent sent it. Sent after the History on
connect, and to every connection of the device whenever the agent's model
changes (after each of its time pings, and invalid after a device Boot).

### History (event 4)

The agent keeps, per device, the most recent entries (256 by default, the
config's `history_size`, 16..4096) for the agent's lifetime, across device
reconnects. A new device connection receives them all, oldest first, in
History events of at most the frame limit, `last` = 1 on the final one; an
empty history is one event with `count` 0.

Entry: `kind u8 | unix_ms u64 | len u8 | body[len]`, with `unix_ms` the
agent's system clock when it recorded the entry.

| Kind (`HistoryKind`) | Body |
|---|---|
| DiagEvent 1 | the `diag_event` payload as the device sent it |
| Arm 2 | `state u8 \| last_disarm_reason u8`, a change of the status stream's arm state |
| Lease 3 | a Lease event body |
| Online 4 | `device_id[6]` |
| Offline 5 | empty |

## The lease

- **Holder only:** `ARM` (system op 2), `CMD_CONFIG`, `CMD_CLEAR` and
  command-group frames. Anyone else is answered `Busy/LeaseRequired`; their
  command frames are dropped and counted.
- **DISARM** (fast path 0x03 or system op 3) is forwarded from every client at
  once, always on the fast path. Only if the agent's link cannot take another
  DISARM (ordinary requests fill its queue and a DISARM is already in flight)
  does one wait, at most one per connection, and those waiting go out as one
  DISARM when the one in flight completes.
- **Exclusive operations** need the lease, or a device whose lease nobody
  holds, with no handoff running, that its last status frame showed not armed
  (before the first status frame after an open or a Boot the device counts as
  disarmed: the agent disarmed it on open, and a Boot leaves it disarmed).
  Otherwise they are answered `Busy/LeaseRequired`.

  | Family | Ops |
  |---|---|
  | System | SAVE, REVERT, FACTORY_RESET, REBOOT, EXIT_SAFE_MODE, SET_NAME, LINK_CONFIRM (reserved), BenchStall and BenchCrash (test builds only) |
  | Layout | ADD_INSTANCE, REMOVE_INSTANCE, SET_LABEL, DISCARD_PENDING |
  | Capture | CONFIG, ARM, ABORT, TRIGGER |
  | Testgen | START, STOP |
  | Action | RUN |
  | OTA | BEGIN, WRITE, FINISH, ABORT, CONFIRM |

  Any other op of the layout, capture, testgen, action and OTA families
  (one a later core version adds) needs the lease too, until this table
  names it open.

- **Open to every client:** every read (GET_INFO, describe, param GET and
  GET_PENDING, LIST, capture STATUS and READ, testgen STATUS, OTA STATUS,
  diag queries), param SET (live or staged), ACK_FAULT, diag CLEAR and the
  subscription-group ops.
- **The end of a lease** (LeaseRelease, a LeaseTake by another client, or the
  holder's connection closing): the agent stops forwarding the old holder's
  holder-only frames at once, sends DISARM on the fast path, and when that
  completes (answered or not) clears the old holder's command groups. A CLEAR
  that fails leaves the slot reserved; the agent retries it every 2 s. A Take
  is granted only after that DISARM and the CLEARs have completed.
- **ARM after a handoff:** while a command slot of an earlier holder is still
  configured on the device, the holder's ARM is answered `Busy/None` without
  reaching the device (that group would count towards ARM).
- **An ARM in flight** when a lease ends is followed, once it completes, by
  another DISARM, and the CLEARs wait for it. Whenever the status stream shows
  the device armed while nobody holds the lease, the agent sends DISARM again
  (one at a time).
- A Take when nobody holds the lease sends no DISARM. The lease survives a
  device Boot (the device is disarmed and has lost its groups); it ends when
  the device goes offline, because every connection is closed.

## Routing on a device connection

Frames from the client:

| Frame | What the agent does |
|---|---|
| `core_command` 0x01 | Checks the core header (a wrong version is answered as a device would: seq 0, family and op 0xFF, `InvalidArgument/None`). Then the lease rules, then the group rules below; everything else is forwarded on the device's link with a new `seq`, and the reply is sent back with the client's `seq`. |
| `disarm` 0x03 | Always forwarded at once on the fast path; the reply goes back with the client's `seq`. |
| `diag_query` 0x23 | Forwarded with a new `seq`, the reply sent back with the client's. |
| `time_request` 0x02 | Dropped and counted. The agent alone pings the device; clients ping the agent (TimePing). |
| command group 0xA0–0xBF | From the holder, for one of its command groups: sent with the device slot's channel (payload unchanged). Otherwise dropped and counted. |
| `agent_command` 0x30 | The ops above; Hello is refused as above. |
| anything else | Dropped and counted. |

When the device does not answer, the client gets no reply either and its own
timeout applies; a mutation stays uncertain, as on a direct link. When the
agent's queue toward the device is full, a request is answered `Busy/None` (a
diag query: result Busy) without reaching the device.

Frames from the device:

| Frame | Goes to |
|---|---|
| `status_stream` 0x80, `diag_event` 0xC0, `core_event` 0xC1 | every device connection |
| subscription groups 0x81–0x9F | the client that owns the group, on its own channel (below), if the epoch is current |
| replies 0x41, 0x63 | the client whose request it answers |
| `time_response` 0x42 | nobody |

### Groups

Group ids are device resources; the agent gives each client its own view.

- A client sees subscription ids 0..S-1 and command ids 0..C-1, where S and C
  are the device's pool sizes (`limits.sub_groups`, `limits.cmd_groups`).
- A CONFIG (`SUB_CONFIG`, or the holder's `CMD_CONFIG`) on a client id with no
  device slot takes the lowest device slot of that kind no client owns; with
  none left it is answered `InvalidArgument/Capacity` (the same answer as an
  id out of range). A refused CONFIG on a newly taken slot gives the slot
  back. The agent rewrites the group id in requests and the channel in
  replies, so the client sees `channel = 0x81 + its id` (subscriptions) or
  `0xA0 + its id` (commands).
- `SUB_ENABLE` on a client id with no slot is answered
  `InvalidArgument/UnknownRef`, as for an unconfigured slot. A CLEAR on one is
  answered Ok with the last epoch that client saw for that id (0 if none), as
  clearing an empty slot is on a device. An accepted CLEAR frees the slot.
- `LIST` answers with the client's own view: its slots of both kinds with
  their device configuration (group id rewritten), every other id empty
  (flags 0, loop 0xFF, divider 1, n 0, the last epoch the client saw). Pages
  follow the device rule in [core.md](core.md), "Group".
- A stream frame from a device subscription slot goes only to its owner, and
  only when its epoch equals the epoch of the owner's last accepted mutation
  of that slot. Other frames are dropped and counted. A command frame keeps
  the epoch the client put in it; the device refuses a stale one.
- When a client disconnects or loses the lease, the agent clears its slots of
  that kind on the device (subscriptions on disconnect, command groups when
  the lease ends). A slot whose CLEAR fails is not handed out again until a
  later CLEAR succeeds.
- On a device Boot every slot is free again (the device lost its groups);
  clients learn of the Boot from the diag Boot event, as on a direct link.

## Time

- The agent alone pings the device (time protocol, [time.md](time.md)) and
  fits its clock: the ClockModel event.
- A client fits its own monotonic clock to the agent's from TimePing answers,
  as it would fit a device: each answer is one sample with the agent's time as
  both device receive and transmit time.
- The client composes the two fits into device time to its own clock: valid
  only while both are, slopes multiplied, residuals and round trips added.
  Mapped timestamps therefore keep working for clients on the agent's host and
  on another one.

## Device lifecycle

- The agent opens each configured device and retries a failed or lost one every
  2 s, without affecting the others.
- On every open, before it accepts clients for that device, the agent reads
  GET_INFO (a core version other than its own keeps the device offline, with a
  log line), sends DISARM, and clears every configured subscription and command
  group, so nothing left by an earlier owner keeps streaming or commanding.
- When the link fails, the device goes offline: its connections get a Closing
  event (DeviceOffline) and are closed; the lobby shows it offline. History
  records Online and Offline.

## Discovery

An agent that listens on a LAN address advertises itself over mDNS/DNS-SD
(RFC 6762, RFC 6763) so the GUI's Agent tab can list it. IPv4 only. The
records are built and parsed in `libdrivelink`
([discovery.hpp](../../host/libdrivelink/include/drivelink/discovery.hpp));
the agent's responder is `host/agent/src/mdns.cpp`.

- **When.** Only when the bind address is not loopback and the config's
  `advertise` is on (the default). Bound to a specific address, the agent
  advertises that address on the interface holding it; bound to `0.0.0.0` or
  `::`, on every up, multicast-capable, non-loopback IPv4 interface, each
  answering with its own address. Interfaces are scanned again every 10 s, so
  an agent started before DHCP finishes advertises once it has an address.
  If UDP port 5353 cannot be opened, the agent says so on stderr and serves
  TCP as usual.
- **Names.** Service type `_drivelink._tcp.local.`. Instance: the config's
  `name` (1..63 bytes of UTF-8, no `.` and no control characters), by default
  the host name's first label. Target host `drivelink-<hostname>.local.`, the
  agent's own name, so it never clashes with records another responder (such
  as avahi) publishes for the host.
- **Records.** PTR `_drivelink._tcp.local.` → the instance; SRV (priority 0,
  weight 0, the agent's TCP port, the target host); TXT; A (the interface's
  address). TTLs: 120 s for SRV and A, 4500 s for PTR and TXT. SRV, TXT and A
  carry the cache-flush bit.
- **TXT.** `txtvers=1`, `proto=<agent protocol version>` (3), `version=<the
  DriveLink version>`. Nothing about the devices: listing them needs the token
  (ListDevices). A client shows an agent whose `proto` differs as incompatible.
- **Answers.** A PTR question for the service (or for
  `_services._dns-sd._udp.local.`) is answered with the PTR and, as additional
  records, SRV, TXT and A; SRV, TXT and A questions are answered directly. A
  question with the unicast-response bit gets a unicast answer; a query from a
  port other than 5353 gets a legacy unicast answer that echoes its id and
  question with TTLs capped at 10 s; anything else is answered by multicast,
  at most once a second per record set and interface. The agent announces
  twice, 1 s apart, when it starts advertising on an interface, and sends
  goodbyes (TTL 0) when it stops or shuts down. Name-conflict probing is not
  done.
- **Browsing.** A client sends a PTR question for the service with the
  unicast-response bit from an ephemeral port on every interface and collects
  answers for a window (the CLI: `agent-browse --timeout-ms`, 1500 ms by
  default; the GUI: 1 s rounds while the Agent tab is shown, backing off from
  1 s to 60 s). The address to connect to is the answer's source address when
  it is one of the A records, else the first A record.

## Versions

| Date | Change | Agent protocol after it |
|---|---|---|
| 2026-11-19 (H3a-1) | First version: Hello, ListDevices, Closing; routing and subscription groups; an interim rule refusing ARM and command groups | v1 |
| 2026-11-19 (H3a-2) | The lease (LeaseTake, LeaseRelease, Lease event), command groups for the holder, exclusive operations, ClientList, TimePing, ClockModel and History events; the device record gains the holder | v2 |
| 2026-11-19 (H3b-1) | The client kind (`AgentClientKind`): `client_kind` in Hello, `kind` in ClientList, `holder_kind` in the device record and the Lease event (and so in Lease history entries) | v3 |
| 2026-11-20 (H3b-2) | Discovery over mDNS/DNS-SD (no change to the TCP protocol) | v3 |
