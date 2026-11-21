# Plan: host software architecture

Status: architecture target, re-planned 2026-11-14 after the firmware overhaul
(F1–F10, LP) against the firmware as built. The first host slice exists:
`libdrivelink` (transport, session, the core families, the F8 clock estimator
and the F9 OTA service), `drivelink-cli`, `drivelink-vdev` and the superbuild.
H1a-1 adds typed services/errors and moves JSON formatting into the CLI;
H1a-2 adds the discovered device snapshot/reload API and Windows host CI.
H1b adds the asynchronous runtime, live model, group buffers, managed commands
and timestamp mapping. H1c adds device identity. H2 built the local GUI. H3a-1 adds
`drivelink-agent` without its lease ([agent.md](../docs/protocol/agent.md)). Still to build: the
agent lease, clock model and history (H3a-2), remote mode and ROS.

This file is the design record. The steps that build it (T0, H1a–H6, HW), with
a brief per step, are in [host-steps.md](host-steps.md); their state and PRs are
in [host-status.md](host-status.md). Everything runs on the
`host_overhaul` branch.

**Baseline.** The host is written against the firmware on `host_overhaul`
([firmware-architecture.md](firmware-architecture.md) is the design record;
`docs/protocol/` is the wire spec and wins where they differ). That firmware is
core v9, time v1, diag v3, and is:
- self-describing: module types, instances, params, signals, inputs, actions,
  sources and diag codes, all served by the device
- configured through a pending layout that SAVE writes to NVS (A/B) and a
  reboot applies; loops are instances of the built-in `loop` type (0x05) with
  a rate, a `realtime` flag and a discovered core placement enum (`core_0` 0,
  `core_1` 1, `any` 2; new loops default to Any, LP), and each module type
  requires, forbids or accepts a real-time loop
- limited by read-only `limits.*` system params (loops, instances, groups,
  link values), so a host can show "4 of 4 loops" before it tries an op
- streamed through subscription groups (31 channels, 0x81–0x9F) and command
  groups (32 channels, 0xA0–0xBF), preallocated, with epochs; the pool sizes
  are Kconfig (default 8 each) and reported in `limits.*`
- protected by arming (`ARM` needs a configured command group), a command
  watchdog, a safety monitor, the `DISARM` fast path (MSG_TYPE 0x03) and diag
  codes that block arming or disarm
- able to capture signals into PSRAM at the loop rate (capture family, core
  events on 0xC1) and to run one test signal generator per running loop
- stamped with monotonic device timestamps that the host maps to its own
  clock through time protocol v1 pings (0x02 / 0x42)
- updated over the link (disarmed OTA: FINISH prepares, explicit REBOOT
  commits after reply drain, the host CONFIRMs after verifying identity)

H1c added device identity (core v9), the only planned host-phase firmware change. Any other firmware need found while building goes to the user first.

## Goals

1. A GUI on the PC to tune the controllers while the ESP is plugged straight into the PC.
2. A ROS 2 interface on the robot host.
3. Tuning from the PC while the ESP is plugged into the robot host, over the network.
4. Adding a new sensor, actuator or control system needs no host code. The firmware module and its YAML are enough ([docs/adding-a-module.md](../docs/adding-a-module.md)).

The build order follows the goals: the library, then the GUI in local mode,
then the agent and the GUI's remote mode, then ROS.

## Decisions

| Topic | Decision |
|---|---|
| Robot host | Raspberry Pi 5, Ubuntu 26.04 arm64, ROS 2 Lyrical Luth (LTS, supported to May 2031). Not available yet: Pi checks are deferred like the motor and IMU checks. |
| PC GUI | C++ with Qt 6 and QML. A Windows product, built with Qt's MinGW kit. Linux CI also builds it and runs its tests against Ubuntu's Qt, to keep the code portable; there is no Linux packaging. |
| Qt version | Qt 6.12.0 `win64_mingw` on Windows (installed locally, pinned in CI through aqtinstall). Qt 6.10 (Ubuntu 26.04's `6.10.2`) is the API floor, enforced by the Linux CI build. Decided 2026-11-14. |
| Language | The host (libdrivelink, CLI, vdev, GUI, agent, ROS packages) is C++23, with GCC 13.1 (Qt's MinGW) as the feature floor, enforced by host-windows CI. No C++26. `protocol/` keeps C++20 as its declared minimum, because the firmware builds it. Decided 2026-11-14 (T0). |
| Host core | `libdrivelink`, plain C++23 with no Qt and no ROS. Used by the GUI, the agent, the ROS plugin and the CLI. |
| Host API | **Typed** (decided 2026-11-14). The public `libdrivelink` API returns typed C++ results and exposes a device model; nlohmann JSON leaves the public API. JSON formatting moves into `drivelink-cli`, whose readable and `--json` output stays as it is, so the smoke scripts keep working. |
| Errors | Sync calls throw typed exceptions: `drivelink::Error` (the existing kinds) and `DeviceError` with the device result, detail and `outcome_unknown`. Async completions receive `std::expected<T, drivelink::Error>`. The ROS `read()` / `write()` paths are `noexcept`. Decided 2026-11-14. |
| Async | Callbacks on a dispatcher the caller supplies (contract under "Session"). No Qt or Asio in public headers; the agent may use coroutines internally. Decided 2026-11-14. |
| Generic host | The host has no module-specific code. It reads the descriptors (items and diag codes) from the device at every connect and builds its models from them. The descriptor hash in `GET_INFO` tells it when the definitions changed; a disk cache keyed by the hash is deferred until connecting gets slow. It uses the generated headers from `protocol/` only for core and system ids. |
| Device identity | A read-only device id (the factory MAC) and a user-set name in `GET_INFO`, set with a new system op `SET_NAME` (core v9, H1c). The name applies live, sets `nvs_dirty`, is persisted by SAVE and restored by REVERT, and may be set while armed. The host keys recent devices and session files on the id. Decided 2026-11-14; proposal in [todo-device-name.md](archive/todo-device-name.md). |
| Port ownership on the robot | A headless agent (`drivelink-agent`) always owns the ports. The ROS plugin and the remote GUI are its clients; the plugin has no direct serial mode (user decision 2026-11-21). |
| Devices per agent | One agent serves several ESPs, one serial port each, every one with its own session, I/O, lease, groups, clock model and history, so one failing port doesn't affect the others. A client opens one TCP connection per device and names it in the hello by its config alias (for example `base`, `arm`) or its device id; after the hello the connection carries that device's frames unchanged. The lease is per device. Decided 2026-11-14. |
| Port ownership on the bench | The GUI opens the serial port itself through `libdrivelink`. No agent is needed on Windows. |
| Agent API | The device protocol over TCP, plus a few agent-only messages. Clients use the same frames as over serial. gRPC is out because of MinGW, and Zenoh would add a Rust toolchain. |
| Agent access | Listens on `127.0.0.1` by default; a LAN address is opt-in in its config. Every client sends the agent's shared token (one per agent, not per device) in the hello, and a wrong token is refused before any device frame. No TLS: a trusted lab network is assumed. Decided 2026-11-14. |
| Agent discovery | With LAN listening on, the agent advertises itself over mDNS/DNS-SD, and the GUI's Agent tab lists what it finds beside typed addresses and recent devices. Decided 2026-11-14. |
| Agent deployment | Debian packages only, for amd64 and arm64: `drivelink-agent` (the agent, systemd unit, udev rules, config and token; Recommends `drivelink-cli`) and `drivelink-cli` (CLI and vdev, no Qt) for Ubuntu 26.04 and 24.04, and `drivelink` (the GUI, Depends `drivelink-cli`) for 26.04 only (24.04's Qt 6.4 is below the floor). There is no from-source install script: a source build runs the packaging script on the Pi and installs the result. After install the agent runs as a `drivelink` user, listens on loopback with a generated token, and serves no devices until `agent.json` lists them. An upgrade never restarts the running agent, since stopping it DISARMs every device; the user restarts it. The ESP's main link is the Pi's GPIO UART; USB-attached ESPs get stable names from udev. Each package's version carries its release (`<VERSION>~ubuntu24.04`, `~ubuntu26.04`; user decision 2026-11-20), so the two builds differ and the older release sorts lower. H3c builds the packages and attaches them to the tag release; H3d publishes them in a signed apt repository on GitHub Pages (tags only, one suite per Ubuntu release by codename); v0.1.0, tagged on `host_overhaul` after H3d, is the first release (user decision 2026-11-21). Decided 2026-11-14, re-planned 2026-11-20 (H3c and H5 merged, H3d added). |
| Device actions over the agent | Actions (including paused actions with blobs, such as IMU calibration apply) are exclusive operations: they need the lease, or a disarmed device with nobody holding it. Live param writes stay open to every client. Decided 2026-11-14. |
| Clock | `libdrivelink` runs the time pings and fits offset and drift (the F8 `ClockEstimator`). The agent owns the clock model and sends it to its clients. |
| Configuration | Lives on the ESP (NVS). The host reads it and writes it, and never stores it. Host-side files hold only GUI settings, recent devices, tuning sessions and calibration exports. |
| Transport | UART at 921600 baud, behind a transport interface. USB can be added later without changes above the transport. No baud-rate change is planned. |
| Python bindings | Not in this plan. The CLI's `--json` stays the scripting interface, and `firmware/test/uart_comm/protocol.py` stays for HIL. Decided 2026-11-14. |
| Repo and branches | One repo. The firmware is in `firmware/`, definitions and codecs shared with the host in `protocol/`. The host steps and the HW campaign run on `host_overhaul`, cut from `main` after F10; `main` stays frozen until `host_overhaul` merges (H6). HW fixes to the firmware also go to `host_overhaul`. |
| License | Proprietary, all rights reserved (root `LICENSE`, `package.xml` `Proprietary`). The repo is public to read only. Distributed artifacts (the installer, the `.deb` packages, the firmware package) ship third-party notices for what they bundle (the firmware's: ESP-IDF and the third-party code it links, `firmware/notices/`; added 2026-11-19): Qt (LGPLv3, dynamically linked), Inter and JetBrains Mono (OFL), Lucide (ISC), Asio (BSL-1.0), nlohmann JSON (MIT) and any later dependency. Decided 2026-11-14. |
| Installer signing | Unsigned for now (SmartScreen shows an unknown publisher). Signing can be added to the CI job later. Decided 2026-11-14. |
| Versioning | One semver version for the whole repo in a root `VERSION` file, read by the firmware (`PROJECT_VER`), the CMake projects, the `package.xml` files, the installer and the `.deb`; `validate.py` checks they match, and dev builds add the `git describe` suffix. Introduced in H2g; H6 tags `vX.Y.Z` on `main`. Decided 2026-11-14. The host compares the core protocol version and the descriptor hash, and refuses to connect on a protocol mismatch with a clear message. Every wire change bumps the version byte of the protocol it changes; the agent protocol has its own version in its hello. |

## Architecture

```
 PC (Windows)                          Robot host (Raspberry Pi 5, Ubuntu 26.04, ROS 2 Lyrical)
 ┌───────────────────────┐   TCP       ┌──────────────────────────────────────────────┐
 │ drivelink-gui         │◄───────────►│ drivelink-agent                              │
 │  (Qt/QML)             │  token,     │  libdrivelink: serial link, session,         │
 │  libdrivelink         │  mDNS       │  time model, device model, arbitration       │
 │   ├ LocalDevice ──┐   │             │         ▲ TCP on localhost                   │
 │   └ RemoteDevice  │   │             │ drivelink_hardware (ros2_control plugin)     │
 └───────────────────│───┘             └──────────────────────┬───────────────────────┘
                     │ serial                                 │ UART 921600
               (bench mode)                                   ▼
                     ▼                                     ESP32-S3
                 ESP32-S3

 CI and development: drivelink-vdev (the firmware core built natively, with mock modules)
 stands in for the ESP on TCP or a pty. On the PC, an agent in WSL can own the bench
 board's CH343 through usbipd, so remote mode is tested before a Pi is ready.
```

### `libdrivelink` (host core)

**What exists (F3c–F9, LP, H1a-1/2, H1b).** Session has a bounded async
request engine, typed services through a serial executor, synchronous wrappers,
exactly-once dispatched completions and emergency DISARM admission/TX priority.
DeviceModel publishes immutable discovered snapshots, follows events and reloads
after Boot. Subscription seqlocks/rings are pulled; managed command queues own
prefix-only keepalives. I/O owns the periodic clock fit and timestamp mappings.
The CLI keeps its output/codes, adding production mapped fields only to time
JSON. Details, lifetimes, loss counters and bounds are in the library README.
The rules are in [host/CLAUDE.md](../host/CLAUDE.md). H1c adds factory identity and a live device name, committed with the A/B layout.

**Target (H1a, H1b, H1c):**

- **Transport.**
  - `SerialTransport` uses standalone Asio and works on Linux and MinGW (kept: bootstrap lines stay disabled so opening the port doesn't reset the board).
  - `TcpTransport` connects to the agent or to `drivelink-vdev`. Toward an agent it sends the hello with the token first (H3a).
  - `UsbTransport` comes later, if needed.
- **Link.** Frame encoding and the frame scanner come from `protocol/`: a `u16` LEN with a header CRC8, and the protocol constant `MAX_PAYLOAD` (1024) as the scanner's hard bound before `GET_INFO` gives the device's `max_payload`.
- **Errors (H1a).** `drivelink::Error` keeps its kinds (usage, connection, protocol, timeout). `DeviceError` adds the device's result and detail, the family and op, and `outcome_unknown` (a mutation that was sent and not answered). Sync calls throw; async completions carry the error as a value.
- **Session (async in H1b).** As today, plus an asynchronous request path for the GUI and the agent:
  - `Completion<T> = std::move_only_function<void(std::expected<T, Error>)>`. The dispatcher is set per session: the GUI posts a queued Qt invoke, the agent posts to its strand.
  - Every request completes exactly once (success, refusal, timeout, close or cancel), through the dispatcher, never inline and never on the I/O thread. Completions arrive in reply order.
  - Cancel stops waiting; it doesn't undo. A mutation already sent completes as `outcome_unknown`. Close fails every pending request.
  - The sync facade wraps the async path; the CLI and tests keep using it.
  - Bulk transfers (descriptors, capture reads, OTA writes) are paged with a bounded number of chunks in flight; OTA WRITE keeps its own retry exception.
  - Model, status and diag changes are dispatched event callbacks. High-rate group data is pulled, not pushed (see "Threading").
- **Time (H1b).**
  - Sends a ping on `0x02` every 1 s, and faster during the first few seconds (F8 has the single-shot ping and the estimator).
  - Keeps the pings with the smallest round trip and fits ESP time to host time (offset and drift) over a sliding window, restarting on a Boot event.
  - Gives every timestamped item (group frames, status, capture, events) both the ESP µs and the mapped host time, with the estimator's uncertainty.
  - Over the agent, a client takes the agent's clock model instead of pinging (H3a).
- **Device model** (typed; the snapshot in H1a-2, event-driven updates in H1b).
  - At connect, or after a Boot event, it:
    1. reads `GET_INFO`: versions, build id, descriptor hash, `max_payload`, capture capacity, flags (pending reboot, layout invalid, config from NVS, safe mode), the running loops and, from H1c, the device id and name
    2. checks the core protocol version, and the `safe_mode` flag
    3. reads the descriptors: types (with their realtime requirement), items and diag codes
    4. lists the configured and the pending instances, including the loop instances
    5. reads every param, including the `limits.*` system params
  - This builds a tree of type → instance (label, loop, state) → params, signals, inputs, sources and actions, with typed values, units, ranges, enum names and flags (`reboot_required`, `locked_while_armed`, `testgen`, GPIO, `ref` wiring).
  - It follows the arm state, the last disarm reason, the pending-reboot and NVS-dirty flags, the per-loop overruns, the capture and testgen state and the active faults through the status stream, core events and diag events.
  - The pending layout is a second view of the tree: added, removed and relabelled instances, applied by SAVE and a reboot.
  - Loop ids can have gaps: everything is keyed by id, never by position.
- **Services** (typed wrappers of the firmware families):
  - param batches, including atomic multi-param writes within one loop (cross-loop batches are refused by the device)
  - layout: add (with a loop, or the lowest compatible one), remove (refused with `Referenced` while dependents exist), label, discard; save, revert, factory reset, reboot, exit safe mode
  - group setup: subscription and command groups with epochs, bound to any loop
  - capture (configure, trigger kinds, arm, status, read, decode into per-signal arrays) and test signals (per loop: step, square, ramp, sine, linear and log chirp; stop and status)
  - actions, including paused actions with blob payloads
  - OTA (stream and prepare the image; explicit reboot commits it after reply drain; reconnect, verify full running build identity and confirm; reconcile uncertain outcomes with STATUS without automatically repeating REBOOT)
  - diag (decode events, query entries, name codes from the code descriptors the device serves)
  - arming (arm with the refusal detail reported, `DISARM` through the fast-path MSG_TYPE, `ACK_FAULT`)
  - command groups with prefix-only keepalives; NaN-to-hold mapping and automatic keepalives in the client (H1b), since the device rejects non-finite values and only a valid frame feeds the watchdog
  - device identity (H1c): read id and name, set name
  - agent session (H3a): hello and token, lease request and release, lease events, clock model, client list, history
- **Tuning math** (H2e, pure, unit-tested, in the library so the CLI can use it too): run metrics (rise time, overshoot, settling time, steady-state error) and plant identification (a first-order-plus-deadtime fit to an open-loop duty capture, with suggested starting gains).
- **Threading.**
  - One I/O thread (Asio) does all the reading and writing.
  - Clients get events through the dispatcher; internal queues are bounded and count what they drop.
  - The latest value of each subscription group sits in a lock-free single-writer buffer (seqlock), so the ROS `read()` never blocks.
  - Consumers that need every frame (GUI plots) pull from a bounded per-group sample ring that drops oldest and counts losses.
  - Command group frames are queued without blocking.

### `drivelink-cli`

Stays the scripting and test front end. It moves onto the typed API in H1a and
owns all JSON formatting; its output formats and exit codes do not change, so
the smoke scripts under `firmware/test/uart_comm/` and `host/test/` keep
working. It gains a `name` command (H1c) and agent options (`--token`, lease
commands) in H3a. Python bindings are not planned.

### `drivelink-agent` (robot host)

- Owns the serial ports, runs as one systemd service, and keeps working while ROS is down. Linux only.
- **Devices.** The config lists the devices, each with an alias, a serial device and a baud. Each device has its own `Session`, I/O, device model, lease, groups, clock model and history; a device whose port fails or disappears is reported as offline and retried without affecting the others.
- **Access.** Listens on `127.0.0.1` unless its config enables a LAN address. A client's first message is the agent hello: agent protocol version, token and the device it wants (alias or device id). A wrong token or version, or an unknown device, is refused before any device frame is forwarded. A hello without a device opens a lobby connection that can only list the devices (alias, device id, name, port, online state, lease holder); the GUI's Agent tab uses it. With LAN listening on, the agent advertises itself over mDNS/DNS-SD.
- **Config.** One file: bind address and port, token, history sizes, and the device list. The `drivelink-agent` package installs it as a conffile, `/etc/drivelink/agent.json` (H3c); command-line flags override it.
- Clients speak the device protocol over TCP, one connection per device. Everything below is per device. The agent arbitrates between the clients of a device:
  - **`seq`.** It rewrites each client's `seq` before forwarding a request. It maps the response back and routes it to the client that sent the request.
  - **Groups.** Group ids are device resources (the pool sizes are in `limits.*`). The agent gives each client its own groups out of the device's pool, rewrites the group ids and stream channels both ways, and sends each client only its own subscription streams. It tracks each group's epoch, so frames left over from a group's previous owner are dropped. The status stream, core events and diag events go to every client.
  - **Time.** The agent alone pings the ESP and fits its clock; it sends clients that model (device time to the agent's monotonic clock) as an agent event whenever it changes. Clients never ping the device: they ping the agent (an agent op answering the agent's clock), fit their own clock to it, and compose the two, so mapped timestamps hold on the agent's host and on another one (user decision 2026-11-19, H3a-2).
  - **Control lease.** Only one client at a time may send `ARM`, configure command groups or send command frames. While ROS's controllers are active, ROS holds the lease. The GUI takes it explicitly to tune.
    - **Any client may disarm**, with or without the lease: the agent forwards `DISARM` (fast path or system op) from every client at once. Decided 2026-11-12 ([gui-design.md](gui-design.md)).
    - A lease change (Take preempts), a release, or the lease holder disconnecting, makes the agent stop the old holder's command frames at once, send `DISARM` on the fast-path MSG_TYPE and then clear the old holder's command groups; a Take is granted after that. That leaves the device plain Disarmed, not in Fault. ARM waits while an earlier holder's command group is still on the device (user decisions 2026-11-19).
    - The previous holder gets a lease event naming it as the previous holder. ROS recovers as described under `drivelink_hardware`.
  - **Exclusive operations.** Capture, test signals, actions, `SAVE`, `REVERT`, `FACTORY_RESET`, `REBOOT`, `EXIT_SAFE_MODE` (added 2026-11-19), `SET_NAME`, layout ops and OTA need the lease, or need the device to be disarmed with nobody holding the lease. Their reads (STATUS, READ, lists), `ACK_FAULT` and diag queries stay open.
  - **Param writes.** Any client may write live params at any time. Batches are atomic on the device. This stays allowed while ROS holds the lease; the GUI warns before such an edit.
  - **History.** The agent keeps recent diag events, arm-state changes, lease changes and the device going online or offline (256 entries per device by default), and sends them to a client when it connects. It keeps no captures.
- **Agent-only messages** are `agent_command` 0x30, `agent_response` 0x70 and `agent_event` 0xF0 (H3a-1), with ops and events: hello, device list and the Closing event (H3a-1); lease take and release, the lease event, client list, time ping, clock model and history events (H3a-2, agent protocol v2). They never reach the device. Agent refusals use shared Detail values (54..58), which the firmware never sends. Their spec is `docs/protocol/agent.md`.
- **Built in H3a-1:** one agent thread owns sockets, routing and each device's group map; each device has its own `Session` (seq rewriting, timeouts, a retired-ID quarantine for links that never reconnect). Opening a device sends DISARM and clears every group before clients reach it.
- **Built in H3a-2:** the lease is a pure state machine (free, held, ending with an optional waiting Take); command groups are virtualized like subscriptions; the agent tracks the arm state from the status stream for the exclusive-operation rule. An ARM still in flight when a lease ends is followed by another DISARM before the cleanup finishes.

### `drivelink-gui` (Qt/QML)

- Qt 6.12.0 with Qt's MinGW kit on Windows; Qt 6.10 is the API floor, which the Linux CI build against Ubuntu 26.04's Qt enforces.
- The screens follow the Figma design ([gui-design.md](gui-design.md), [gui-design-status.md](archive/gui-design-status.md)), including G7b's loops-and-sources rework and LP's placement enum (Any core is the Add loop default). G8 is folded into H2a: no clickable prototype; the tokens are exported and the open handoff issues are settled in code.
- **Theme.** Figma's variables are exported to `tokens.json` in the repo and a script writes the `Theme` singleton with a dark and a light set; Figma stays the source of truth. Inter and JetBrains Mono (OFL) and the Lucide icons used (ISC) are bundled in the Qt resources.
- **Style.** A custom DriveLink style built on the Basic style; each G2 component maps to one QML control (the mapping table in Figma).
- **C++ backend:**
  - A device connection, either local (serial, or TCP to vdev) or remote (agent), behind one interface. Library completions and events reach QML through the session dispatcher (queued invokes onto the GUI thread).
  - Its own command groups cover running, discovered inputs, partitioned by loop and the device's capacities; with no inputs, an empty group supplies keepalives. H2a-2 creates prefix-only keepalive handles, leaving input values unchanged: descriptor defaults are not semantic holds. H2d-2 added explicit finite holds for value commands, by the hold rule (user decision 2026-11-17): only numeric inputs whose descriptor range is finite and includes 0 are commanded; each loop's inputs split at connect into value groups (those inputs, hold 0, the value the firmware sets at ARM) and keepalive-only groups that never carry values; every value sent is a shown slider value or 0. A live param write to a commanded instance discards its queued values and holds it at 0 until the write ends, then sends a fresh frame (user decision 2026-11-18). A disarmed direct connection sets groups up at connect; an already-armed connection observes until explicit DISARM acknowledgement, even if its watchdog expires during discovery. It never adopts existing groups. A remote connection starts as an observer and sets groups up only after receiving the control lease. User decision 2026-11-15.
  - Models built from the device model:
    - an instance tree (type, label, loop, health), with loops as instances
    - a param table per instance (typed editors, units, ranges, enums, a pending-reboot marker, a dirty marker)
    - a signal list, including sources' value and age signals
    - action forms
- **QML:** entirely generic, with no screen for any particular module. A module can still add an optional custom panel under `host/gui/modules/<type>/`, matched by type name and compiled into the resources (the IMU calibration panel is the first).
- **Plots:** a custom `QQuickItem` that draws line strips through the scene graph from a C++ ring buffer fed by the library's per-group sample rings. Live plots use subscription groups the GUI sets up itself; captures are plotted from uploaded buffers.
- **Screens:**
  - **Connect:** a serial port (or a TCP address for vdev), or an agent (found by mDNS or typed, with its token) and then one of its devices from the agent's device list; recent devices keyed by device id. The device list is not in the G6 frames; H3b adds it.
  - **Device:** info (name, id, versions), loops and their rates and overruns, limits, arm/disarm with the reasons `ARM` was refused, fault acknowledgement, faults (diag entries with the names and descriptions the device serves), save/revert, reboot, OTA, rename, and a safe-mode banner with `EXIT_SAFE_MODE`.
  - **Layout:** add or remove instances (including loops), the loop each module runs in, wiring (`Ref` params with pickers), sources, labels, pending-reboot banner.
  - **Tune:**
    1. Pick a controller instance and edit its gains live.
    2. Choose a test signal and the capture signals.
    3. Run, then overlay the results against earlier runs.
    4. Save or revert.

    Gains are the live params the descriptor flags `tune` (core v10
    `ItemFlag::Tune`), grouped by dotted prefix under a tune-flagged enum
    ([adding-a-module.md](../docs/adding-a-module.md)). A run suspends the
    controller's manual commands at 0 like a live write; when the signal has
    ended on the device the controller's value group sends a fresh frame, so a
    commandable target returns to 0 (user decision 2026-11-18).
  - **Monitor:** free-form live plots of any signals, and a manual command panel that sets inputs by hand while armed.
- **Files on the PC:**
  - Tuning sessions (runs: capture, the params used and the test signal; gain history) are one versioned JSON file each, keyed by device id, with capture columns stored as base64 little-endian arrays. Each run exports to CSV.
  - IMU calibration exports get their own versioned JSON format (H2f).
  - App settings and recent devices use `QSettings`.
- **Tests.** C++ backend and model tests against vdev; Qt Quick Test for key controls and flows, offscreen, in CI on both OSes; and a scenario-screenshot tool that renders named screens from vdev scenarios (and, where a step says so, from the bench board) to PNG at 2560×1440 and 1920×1080, dark and light, for comparison with the Figma frames. vdev gains scenario hooks (inject faults, safe mode, a stalled link, a reboot, slow replies) for this.
- **Distribution.** An Inno Setup installer, built by host-windows CI from a `windeployqt` staging directory (H2g).

### `drivelink_hardware` (ROS 2)

- A ros2_control `SystemInterface` for ROS 2 Lyrical (Ubuntu 26.04) and Jazzy (Ubuntu 24.04), using only the ros2_control API both share. Built with colcon from source, and shipped as bloom `.deb` files in the apt repository (H4d). Re-planned with the user 2026-11-21 (H4a to H4d in [host-steps.md](host-steps.md)).
- **Parameters:**
  - how to connect, always through the agent: `agent` (default `tcp://127.0.0.1:47620`), `token_file` (default `/etc/drivelink/token`, readable by the `drivelink` group) or an inline `token` for tests, and `device` (alias or device id). The plugin never opens a serial port. Its Hello sends `client_kind` Ros. One hardware component per device; a robot with two ESPs has two components.
  - `take_lease`: by default `on_activate` fails, naming the holder, when another client holds the lease; `preempt` takes it anyway (LeaseTake always preempts on the wire).
  - for each joint: the label of its controller instance. Interfaces map by convention: state `position` → `position`, `velocity` → `velocity`, `duty` → `duty`; command `position` → `position_sp`, `velocity` → `velocity_sp`, `duty` → `duty_sp`. `duty` is a custom interface (open loop, not effort). A parameter per interface overrides the item name, so another controller type needs no code.
  - for each sensor: the label of its instance. The IMU follows the ROS convention by default: orientation ← `quaternion.*`, angular velocity ← `gyro.*`, linear acceleration ← `accel.*` (gravity included, as `sensor_msgs/Imu` expects), overridable the same way.
- **`on_configure`:**
  1. Connect and load the device model. Safe mode or a pending reboot is a clear error.
  2. Resolve every label and item name to a `Ref`. Anything missing, wrongly typed or failed is a clear error.
- **`on_activate`:**
  1. Check the device record's lease holder (see `take_lease`), then take the lease.
  2. Configure one subscription group per loop that holds a mapped signal, and one command group per loop that holds a command input (every declared command input; CMD_CONFIG is refused while armed, so the group is fixed until deactivation).
  3. Stay disarmed. The command group sends nothing until a claim: it has no keepalives (H4b, user decision 2026-11-21), so only `write()` feeds the device watchdog and a stalled controller_manager loop disarms the device within `watchdog_ms`.
- **`prepare_command_mode_switch` / `perform_command_mode_switch`** (H4b):
  1. A claiming `prepare` (off the real-time loop, blocking, bounded by `timeout_ms`) plans the switch: one command interface per joint at a time; joints may be in different modes. It sets the position holds from a measured position (the claimed joints', and at ARM every joint's; no readable position refuses the switch). While armed it then waits two `write()` cycles and discards the frames queued before the new holds, so no old value reaches the controller after its mode changes. Then it writes each controller's `mode` param in one batch. The firmware switches modes bumplessly and holds until a fresh command, so this works while armed and a mode change never DISARMs. A stop-only `prepare` only validates (ros2_control also calls it from the real-time loop); `perform` applies stops from its own list.
  2. When the first command interface is claimed, `prepare` arms: `write()` starts sending hold frames, the plugin waits until the status stream shows the device disarmed, sends `ARM`, and waits until it shows it armed. A refused ARM fails the switch with the refusal in the log; an ARM without an answer is followed by `DISARM`.
  3. `perform` (real time) only applies the claims; a new claim starts from NaN. When the last one is released, a plugin worker thread sends `DISARM`. State-only use (broadcasters) never arms.
  4. A claim prepared on the real-time thread (ros2_control's fallback controllers) is refused, and a prepared claim not performed within `timeout_ms`, or followed by another claim's prepare, makes `read()` return `ERROR` (user decisions 2026-11-21). `is_async` components are refused (the real-time thread is the one running `read()`).
- **`on_deactivate`:** `DISARM`, then release the lease.
- **`read()`** (`noexcept`): copies the latest group values out of the seqlock buffers. A group whose newest sample (its device time mapped through the agent's clock model) is older than `stale_timeout_ms` (default ten group periods, at least 100 ms; 0 off) makes it return `ERROR` (user decision 2026-11-21, H4a). Each loop's group runs at its loop rate divided by `max(1, floor(loop rate / state_rate_hz))`.
- **`write()`** (`noexcept`): while armed (or arming, or until the DISARM after the last release) queues one command group frame with every declared input and never blocks.
  - A NaN or infinite command value (the ros2_control default before a controller writes) is sent as hold: zero velocity or duty, or for position the last position sent, which starts at the measured position when the claim begins or ends. Unclaimed inputs carry the same holds. The firmware rejects non-finite frames, and a rejected frame doesn't feed the watchdog.
- **Disarm while armed** (H4b): the plugin's observer decodes the status stream. After its own ARM is seen, a disarmed status raises a fault with the status's `last_disarm_reason`, so `read()` returns `ERROR` and `on_error` logs the reason; a status stream silent for three periods (at least 300 ms) while armed does too.
- **Status and faults:** a diagnostics node in `drivelink_bringup` holds its own agent connection as an observer (no lease, `client_kind` Ros), so it keeps working while the component is inactive or in `ERROR`. It publishes `/diagnostics` from the status stream, diag events, the lease holder and the online state, and offers the services `ack_fault` and `recover`.
- **Losing the device while active.** A disarm (watchdog, fault, a loop stall, or the GUI taking the lease; H4b), a lost lease, the device going offline or rebooting, the agent link dropping or stale data makes `read()` return `ERROR`; `on_error` releases the lease and disconnects, so the component is unconfigured. ros2_control then deactivates the component. Nothing re-arms on its own. Recovery is an explicit act by an operator or a supervisor node:
  1. hand the lease back (the GUI releases it)
  2. call the diagnostics node's `recover`. It acknowledges any fault, reactivates the component through controller_manager and starts inactive controllers from the launch-configured list. Drive controller claims ARM again.

  The documented manual equivalent is `ack_fault` (or the GUI) and `ros2 control set_hardware_component_state`, then the controllers.
- **IMU:** exposed as sensor state interfaces, published with `imu_sensor_broadcaster`. `imu.bno055` runs in a non-real-time loop, so its group is separate from the motor loop's.
- **`drivelink_bringup`:** launch files, controller YAML, the diagnostics node and URDF snippets for an example two-wheel diff-drive robot with an IMU that runs against vdev mocks. The state broadcasters start active and the drive controller starts inactive. The real robot's config is added when it is on the bench.

## Link budget

At 921600 baud 8N1 the link carries 92 160 B/s in each direction. Frame overhead is 9 bytes (including the header CRC8). A subscription group frame is `9 + 12 + 4·k` bytes (version, epoch, counter, u64 timestamp, then the values), and a command group frame is `9 + 4 + 4·k`.

The table below is for ESP → host: motor signals (position, velocity, duty), plus an IMU group of 10 values at 100 Hz, plus the status stream at 10 Hz.

| Motors × 3 signals | 100 Hz | 200 Hz | 500 Hz | 1 kHz |
|---|---|---|---|---|
| 2 (frame 45 B) | 12 % | 17 % | 31 % | 56 % |
| 4 (frame 69 B) | 14 % | 22 % | 44 % | 82 % |
| 8 (frame 117 B) | 20 % | 32 % | 70 % | 134 % |

Host → ESP, one velocity per motor:

| Motors | 100 Hz | 200 Hz | 500 Hz | 1 kHz |
|---|---|---|---|---|
| 4 (frame 29 B) | 3 % | 6 % | 16 % | 31 % |
| 8 (frame 45 B) | 5 % | 10 % | 24 % | 49 % |

What follows from the tables:
- Normal running at 100–200 Hz uses a small share of 921600 baud. A subscription group's divider sets its rate below the loop rate.
- Tuning doesn't depend on the link, because capture records on the device at the full loop rate.
- The control rate itself never uses the link.
- At high rates with many motors, consider 2–3 Mbaud, or a USB transport later. Neither is planned.
- The GUI's signal browser uses the same arithmetic to show what a plot would add, and refuses an add over 100 % (G5).
- A subscription group delivers at most 1 kHz whatever its loop rate: the firmware's link task sends each group's latest sample once per 1 ms, so faster samples overwrite each other and show as counter gaps. The GUI caps a group's rate there (measured on the bench in H2d-1: a 2 kHz loop at divider 1 gave 1000 frames/s, two ticks apart).

## Repository layout

What exists after F10, and what the host steps add (marked with the step). Each directory is created by the step that first puts something in it. No empty placeholder directories.

```
drivelink_v2/
├─ CLAUDE.md  README.md  LICENSE
├─ VERSION                    H2g: the one repo version
├─ firmware/                  ESP-IDF project (CLAUDE.md, README.md)
│  ├─ main/
│  ├─ components/             uart_comm, diag, rtos_util, core_os, core, mod_motor,
│  │                          mod_imu_bno055, mod_mock, motor_hal, rotary_encoder_hal, pid
│  └─ test/
│     ├─ native/              core, module and protocol tests, built natively
│     └─ uart_comm/           HIL test app + pytest, smoke scripts
├─ protocol/                  pure C++20: IDF component + CMake package + package.xml
│  ├─ defs/                   registry.yaml, diag_codes.yaml, system.yaml, loop.yaml, <module type>.yaml
│  ├─ include/drivelink/protocol/   framing, wire_codec, codecs; gen/ (generated)
│  ├─ src/
│  └─ test/
├─ host/
│  ├─ CLAUDE.md  README.md
│  ├─ cmake/                  pinned dependencies
│  ├─ superbuild/             CMakeLists.txt + CMakePresets.json (COLCON_IGNORE)
│  ├─ libdrivelink/           (+ package.xml)
│  ├─ vdev/                   drivelink-vdev: links the firmware core, core_os, mod_mock and the pure motor controller natively
│  ├─ cli/                    drivelink-cli
│  ├─ test/                   host unit and integration tests, hardware smoke scripts
│  ├─ gui/                    H2a (COLCON_IGNORE): app, style, theme (tokens.json + generator), tests, shots tool
│  │  ├─ modules/<type>/      optional custom panels (H2f: imu.bno055)
│  │  └─ installer/           H2g: Inno Setup script
│  ├─ agent/                  H3a (COLCON_IGNORE); deploy/ unit, udev rules, config, package scripts (H3c)
│  └─ ros/                    H4a to H4c (colcon); H4d .debs
│     ├─ drivelink_hardware/
│     └─ drivelink_bringup/
├─ docs/
│  ├─ adding-a-module.md  development.md
│  └─ protocol/               generated tables + hand-written family specs (+ agent.md, H3a)
├─ tools/
│  ├─ gen/                    YAML → C++ / docs generator
│  ├─ validate.py             every check, locally and in CI
│  ├─ bench_fw.py             guarded bench/normal/XiP/OTA-image builds and flashing
│  ├─ setup_linux.sh          H2a: the Linux package list for WSL and CI (ROS added in H4a)
│  └─ wsl_serial.py           H3b: guarded usbipd attach/detach of the CH343 to WSL
├─ plans/                     plans, status, GUI design records; archive/ holds finished step plans
└─ .github/workflows/
```

## Build systems and toolchains

- **Firmware:** `idf.py -C firmware build`. `protocol/` joins through `EXTRA_COMPONENT_DIRS`.
- **`protocol/` and `libdrivelink/`:** one `CMakeLists.txt` each.
  - Under IDF (`ESP_PLATFORM`), `protocol/` calls `idf_component_register`.
  - Otherwise each one is an ordinary library that installs and exports a CMake package (`find_package(drivelink_protocol)`, `find_package(drivelink)`).
  - Each one also has a `package.xml` with `<build_type>cmake</build_type>`, so colcon builds it as a dependency in the same workspace.
- **Host superbuild:** `host/superbuild/CMakeLists.txt` brings in `protocol/` and builds libdrivelink, vdev, cli, gui and agent together (`add_subdirectory` with explicit binary dirs).
  - It lives in its own directory, not at `host/CMakeLists.txt`. colcon treats any directory with a `CMakeLists.txt` that calls `project()` as a package and does not look for packages inside it, so a superbuild at `host/` would hide `host/libdrivelink` and `host/ros/*`.
  - `vdev` compiles the firmware core and the pure modules against `core_os`'s native backend.
  - `CMakePresets.json` has `windows-mingw` (with the GUI), `linux` (with the GUI and the agent) and `linux-no-gui` (the agent and the libraries, for the ROS and agent jobs) presets.
  - The Windows preset names Qt's MinGW compiler explicitly; the agent is not built on Windows.
- **ROS:** `colcon build` from the repo root. `firmware/`, `host/superbuild/`, `host/gui/`, `host/vdev/`, `host/cli/` and `host/agent/` carry `COLCON_IGNORE`. colcon finds `protocol/`, `host/libdrivelink/` and `host/ros/*` through their `package.xml`.
- **Standards:** the host is C++23 with GCC 13.1 as the floor (MinGW 13.1 on Windows, GCC 15 on Ubuntu 26.04); `protocol/` declares C++20. The firmware native tests build with MSYS2 UCRT64 GCC 16.2 from T0 on.
- **MinGW notes:**
  - Never mix compilers. Qt, libdrivelink, vdev and the GUI all build with Qt's MinGW kit.
  - Another `g++` may be on `PATH` (Strawberry Perl; MSYS2 UCRT64 for the native tests after T0). Name the compiler in the preset, and ship the right runtime DLLs with `windeployqt`.
  - Asio on MinGW needs `ws2_32` and `mswsock`, and `_WIN32_WINNT` defined.
- **Dependencies:** keep them few and mostly header-only, version- and SHA256-pinned in `host/cmake/Dependencies.cmake` (standalone Asio, doctest, nlohmann JSON for the CLI; mjansson/mdns 1.4.3 for agent discovery, H3b-2). No vcpkg, because its MinGW support is community-only. Qt comes from Qt's installer (Windows) and Ubuntu's packages (Linux). The generator needs Python 3 and PyYAML.

## Development environment

Decided 2026-11-14. Sessions run in the Claude desktop app on Windows; only builds go to WSL.

- **Windows** (native): ESP-IDF, the firmware native tests, the host with Qt's MinGW (`host-win`), the GUI, flashing and the serial ports.
- **WSL** (the `Ubuntu-26.04` distro): `host-linux`, and later the `agent` and `ros` checks. `tools/validate.py` runs them there from the Windows session, as it does `host-linux` today, with build directories under `~/.cache/drivelink/<worktree>/` in the distro.
- **`tools/setup_linux.sh`** is the one list of Linux packages. It skips anything already installed (apt packages, then from H4a a `ros` profile: ROS Lyrical on 26.04 or Jazzy on 24.04, ros2_control, ros2_controllers, colcon and rosdep). The WSL distro runs it once (by hand, or an agent with sudo approved). CI's host-linux and agent jobs run it in `ubuntu:26.04`, where ROS isn't needed; the ros jobs run it in the `ros:lyrical` and `ros:jazzy` images, where it only adds the extra packages. `ci.yml` holds no package list. It lands in H2a, the first step that needs new Linux packages (Qt 6 for the GUI's Linux build).
- **validate.py checks** for the new areas (`agent` in H3a; `ros` in WSL and `ros-jazzy` in the `ros:jazzy` image through Docker Desktop in H4a, the latter skipped with its fix when Docker is stopped) are built like `host-linux`: each first checks for missing tools and stops with one line naming the exact fix, keeps its output short with full logs in `build/validate/`, reports in `build/validate/summary.json` like every check (PR #66), and is skipped when unchanged.
- **`tools/wsl_serial.py attach|detach|status`** (with `--dry-run`, H3b) hands the CH343 to WSL through usbipd for agent hardware tests, and always gives it back to Windows afterwards. The one-time `usbipd bind` is done by hand in an admin shell.
- **No devcontainer for now.** Revisit if cloud sessions should build the Linux side.
- Later speed-ups, if CI time matters: run the ros job only when `host/ros/`, `host/libdrivelink/` or `protocol/` change, or build a CI image from the setup script.

## Adding a module

The checklist is [docs/adding-a-module.md](../docs/adding-a-module.md). On the host side: nothing in code; optionally a custom GUI panel; for ROS only the joint or sensor mapping in the robot's config.

## CI

Every check is defined once in `tools/validate.py`; CI jobs only provide the toolchain and call it with `--only`, and `python tools/validate.py full` runs the same checks locally.

| Job | Runs on | What | State |
|---|---|---|---|
| firmware | `espressif/idf:v5.5.2` | the main, nolog and HIL test-app builds, zero warnings | exists |
| native-tests | `ubuntu:26.04` container (T0) | generator checks and tests, firmware native and protocol tests | exists; container in T0 |
| host-linux | `ubuntu:26.04` + `setup_linux.sh` | superbuild, libdrivelink/CLI tests, vdev integration, installed-package smoke; GUI build and tests against Qt 6.10 from H2a | exists |
| host-windows | Windows, Qt 6.12.0 `win64_mingw` + MinGW 13.1 via aqtinstall | superbuild, tests, the C++23/GCC 13.1 floor; GUI and Quick Tests from H2a; the Inno Setup installer as an artifact from H2g | exists (H1a-2) |
| agent | `ubuntu:26.04` + `setup_linux.sh` | agent ↔ vdev integration tests | H3a |
| ros | `ros:lyrical` and `ros:jazzy` + `setup_linux.sh ros` | `colcon build` + `colcon test` through `tools/ros_check.sh`: the plugin's unit and integration tests against vdev and the agent (H4a), the full-loop launch test (H4c) | exists (H4a) |
| ros deb | the ROS images, amd64 and arm64 | bloom `.deb` files for both distributions, installed in clean containers with the launch test; tags publish them to the apt repository | H4d |
| deb | `ubuntu:26.04` and `ubuntu:24.04` on `ubuntu-latest` (amd64) and `ubuntu-24.04-arm` (arm64) | `drivelink-cli` and `drivelink-agent` for both releases and architectures, the GUI's `drivelink` for 26.04 on both; installed, upgraded, removed and purged in a clean container | H3c (jobs `linux-package`, `linux-install`) |
| apt repo | `ubuntu:26.04` (`apt-repo`), the `deb` matrix (`apt-install`); `apt-publish` tags only | every run: the repository built with a throwaway key, checked by apt and installed from in clean containers; a tag: the release's packages signed with the real key (environment `apt-repo`) into one suite per Ubuntu release on `gh-pages` | H3d (jobs `apt-repo`, `apt-install`, `apt-publish`) |

## Phases

The steps, their dependencies and briefs are in [host-steps.md](host-steps.md); their state is in [host-status.md](host-status.md).

| Phase | Steps |
|---|---|
| Done | S0, S1, F1–F10, LP: the workflow, the repo restructure, the firmware ([firmware-architecture.md](firmware-architecture.md)) and the first host slice. |
| Tooling | T0: C++23 for the host and the native tests, MSYS2 UCRT64 GCC 16.2 for the native tests, the native-tests job in `ubuntu:26.04`. |
| Library | H1a typed API and device model; H1b async path, events, group buffers, time mapping; H1c device identity (core v9). |
| GUI, local | H2a-1 build and style, H2a-2 shell and backend; H2b Connect and Device; H2c Layout; H2d Monitor; H2e Tune; H2f IMU calibration panel; H2g installer and close-out. |
| Agent and remote | H3a agent; H3b GUI remote mode; H3c agent packages and Pi deployment; H3d signed apt repository. |
| ROS | H4a toolchain and the `drivelink_hardware` state path; H4b the command path; H4c `drivelink_bringup` and diagnostics; H4d ROS `.deb` packages. |
| Finish | H6 cleanup and the merge into `main`. |
| Hardware | HW, whenever hardware arrives. |

The S1 restructure details are in git history (`git show 272575d:plans/host-architecture.md`).

## Open questions

All earlier open questions are decided (live params under a lease, agent history, tuning session files, typed API, Python bindings). New ones are raised with the user by the step that meets them.

## IMU calibration contract

IMU actions and typed calibration params remain descriptor-driven. Read/write
stages configuration, explicit SAVE commits it, and boot restores it. A generic
action form can pass the versioned blob; a custom GUI panel is optional. Vdev
uses `imu.mock` and the shared pure state/codec, without a physical bus backend.
Calibration quality comes from levels, while `calibration.valid` means a staged
blob exists. See `docs/protocol/imu.md`. Through the agent, calibration actions
need the lease (see "Exclusive operations").
