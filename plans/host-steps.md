# Host steps

The steps that build the host in [host-architecture.md](host-architecture.md),
planned with the user on 2026-11-14. Each step runs in one session with
`/overhaul-step` and ends in one PR into `host_overhaul`. The state and PR of
every step are in [host-status.md](host-status.md); this file holds what
each step must do. A step session reads its brief, the architecture sections it
names and the status file, then writes its own plan for approval as the skill
says. A brief is a scope, not a design: the step plan settles the details and
logs its internal choices.

## Rules for every host step

- **Order.** Library, then the GUI in local mode, then the agent and remote mode, then ROS (user decision). A step starts when the steps it depends on are merged. "May run beside" marks a step that can run at the same time in its own session and worktree.
- **Language and toolchains.** The host is C++23 with GCC 13.1 as the floor; `protocol/` stays C++20. Qt 6.12.0 on Windows, Qt 6.10 as the API floor. No module-specific code anywhere in the host.
- **Validation.** `python tools/validate.py full` passes before the PR: `build/validate/summary.json` reports `full_gate_passed` for the revision pushed. Sessions read that report (`docs/development.md`, "JSON report") rather than console output or whole logs. A new test suite becomes a `Check`, and CI calls it by name; new checks (`agent`, `ros`, the version check) appear in the report automatically. CLI output and exit codes stay unchanged unless the step says otherwise; new JSON fields may be added.
- **Docs in the same PR.** `host/README.md`, the READMEs of the parts touched, `host/CLAUDE.md` when a rule changes, `docs/protocol/` for any wire change, `docs/development.md` for new checks or tools.
- **Hardware.** "Bare board" means the session runs it on the bench board after review (mock modules only). Motors, a real IMU, the Pi and the robot are deferred: the step writes and dry-runs the script and adds its row to "Deferred hardware checks" in [host-status.md](host-status.md); results go to [hardware-log.md](hardware-log.md). The bench is described in [docs/bench.md](../docs/bench.md).
- **Review.** Planned and built on Opus 5.5 at high effort. An independent Opus review at high effort is **required** where the table says so (concurrency, wire or persistence, arm/disarm, command paths, lease and auth), and otherwise the session reviews its own diff unless the diff touches those paths.
- **Size.** If a step passes about 4–5k lines, its plan proposes a split into rows (for example H3a-1 and H3a-2); the briefs below name the natural cut where one is likely.

## Overview

| Step | Content | Depends on | Hardware check | Review |
|---|---|---|---|---|
| T0 | Tooling: C++23 for the host and native tests, MSYS2 UCRT64 GCC 16.2 for the native tests, native-tests CI in `ubuntu:26.04` | — | no | self |
| H1a-1 | Typed library: types, services and errors, CLI on the typed API | T0 | bare board: CLI smokes | independent (large diff) |
| H1a-2 | Device model snapshot and host-windows CI | H1a-1 | bare board: CLI smokes | independent |
| H1b | Async path, events into the model, group buffers, command keepalives, periodic time and timestamp mapping | H1a-2 | bare board: time, 1 kHz groups, smokes | independent |
| H1c | Device identity, core v9 (firmware, host, vdev, docs) | H1a-2; may run beside H1b | bare board: identity, name persistence, smokes | independent |
| H2a | GUI foundation: app, Theme from Figma tokens (G8 folded in), style, shell, status bar, DISARM/Esc, Qt adapters, test and screenshot tooling, vdev scenario hooks, `setup_linux.sh` | H1b, H1c | vdev | independent |
| H2b | Connect and Device screens | H2a | bare board: GUI scenarios | independent |
| H2c | Layout screen | H2b | vdev | self |
| H2d | Monitor screen | H2c | vdev | independent |
| H2e | Tune screen, run metrics and plant identification, session files | H2d | vdev; deferred: motor | independent |
| H2f | IMU calibration custom panel | H2c; may run beside H2d/H2e | vdev; deferred: IMU | self |
| H2g | Inno Setup installer (unsigned), root `VERSION`, third-party notices, local GUI close-out | H2e, H2f | bare board: installed GUI | self |
| H3a | Agent for several devices: server, access, per-device arbitration and lease, clock model, history, agent MSG_TYPEs; library agent client; agent check and CI | H1b (planned after H2g) | vdev | independent |
| H3b | GUI remote mode with the agent's device list, mDNS, `wsl_serial.py` | H3a, H2g | PC: agent in WSL owns the bench board | independent |
| H3c | Pi deployment: install script, systemd unit, udev rule, config | H3a; may run beside H3b | deferred: Pi | self |
| H4a | `drivelink_hardware` (ros2_control), ROS in `setup_linux.sh`, ros check and CI | H3a | vdev; deferred: robot | independent |
| H4b | `drivelink_bringup`: example diff-drive + IMU, diagnostics node, launch test | H4a | vdev; deferred: robot | self |
| H5 | Agent `.deb` (arm64) | H3c | deferred: Pi | self |
| H6 | Cleanup, docs, merge `host_overhaul` into `main` | all H steps | bare board: full smoke and GUI | self |
| HW | Hardware campaign: every open deferred row, whenever hardware arrives | — | motors, IMU, Pi, robot | per fix |

## Left to the step plans

Settled by the step's own plan (an internal choice, logged), or put to the user there if it touches the wire, a safety rule or a later step.

| Item | Step |
|---|---|
| Where the device name is stored (its own NVS key or the A/B config); allowed characters; vdev's stable fake id | H1c |
| Faults badge: active faults only, or history too | H2b |
| What the reboot notice lists as lost (the GUI tracks params changed since the last save) | H2b |
| G5 1080p layouts: Monitor plot fitting and paused toolbar; Tune plot column, A/B card, gain history placement | H2d, H2e |
| The custom plot item's performance at 1 kHz on several plots (QtGraphs only if it proves better) | H2d |
| Calibration file format, provenance, Euler convention, cancel and reconnect behaviour | H2f |
| Agent default port, history sizes, retry policy for an offline device | H3a |
| mDNS library, service name and TXT records | H3b |
| Where the GUI keeps agent tokens (`QSettings` or Windows Credential Manager) | H3b |
| The device-list screen on Connect's Agent tab (not in Figma) | H3b |
| Pi UART wiring and how udev rules match each ESP | H3c |
| Whether open HW rows must pass before `host_overhaul` merges into `main`; what of the temporary workflow is removed | H6 |

Not planned: a descriptor disk cache (until connecting gets slow), a USB transport, a higher baud rate, Python bindings, installer signing, a devcontainer.

## T0 Tooling

Runs before the first library step.

- C++23 for the host superbuild (libdrivelink, CLI, vdev) and the firmware native tests; `protocol/` keeps C++20 as its declared minimum.
- The firmware native tests build with MSYS2 UCRT64 GCC 16.2 instead of Strawberry; `tools/validate.py` names the tools explicitly.
- The native-tests CI job runs in an `ubuntu:26.04` container.

## H1a-1 Typed services and CLI

**Depends on** T0. **Architecture:** "`libdrivelink`" (Errors, Services), "`drivelink-cli`".

Scope:
- Typed public API in namespace `drivelink`: values (descriptor-typed scalars and enums), descriptors (types with their realtime requirement, items with kind, type, unit, range, enum names and flags, diag codes), `GET_INFO`, instances (configured and pending), groups, capture, testgen, OTA status, diag entries and events, status frames.
- Errors: `drivelink::Error` kinds kept; `DeviceError` with result, detail, family/op and `outcome_unknown`. Sync calls throw.
- Typed services for every family `Client` covers today, synchronous.
- nlohmann JSON leaves the public headers. The CLI owns all JSON formatting; its readable and `--json` output and exit codes do not change. Capture the current outputs against vdev before the refactor and keep them as golden tests.
- Docs: `host/libdrivelink/README.md`, `host/README.md`, `host/CLAUDE.md` (typed API/error rules), `docs/development.md` (host verification).

Not in scope: the model snapshot and Windows CI (H1a-2); the async path, events, group buffers, time mapping (H1b); identity (H1c).

Tests: unit tests for parsing into the typed structs and error mapping; CLI golden output; the existing integration and installed-package smoke on the typed API.

Hardware check (bare board): the CLI smokes that cover the moved services: `hardware_smoke.py`, motor, IMU, capture and testgen with mocks, loop affinity, and `ota_smoke.py` with an `ota-image` candidate. Restore the board to the "Board state" in [host-status.md](host-status.md).

Review: independent, because every service is rewritten.

User-approved split (2026-11-15): this step and H1a-2 replace H1a. H1b and
H1c wait for both parts. [Approved plan and results](h1a-1-implementation.md).

## H1a-2 Device snapshot and Windows CI

**Depends on** H1a-1. **Architecture:** "Device model", "CI".

Scope:
- The device model as a snapshot: the connect sequence (info, version and safe-mode check, descriptors, configured and pending instances, every param including `limits.*`) and an explicit reload. Event-driven updates come in H1b.
- The host-windows CI job: a Windows runner with Qt's MinGW 13.1 (and Qt 6.12.0, needed from H2a) through aqtinstall, pinned and cached, running `validate.py --only host-win`. It enforces the GCC 13.1 C++23 floor.
- Build on H1a-1's typed services and descriptors; JSON stays in CLI.
- Update the model's API README, host/development docs and status/handoff.

Tests: snapshot construction and explicit reload against vdev, including safe
mode, pending/configured layouts, sparse IDs, discovered enums and every param
including limits; installed consumers and the Windows CI check.

Hardware check: bare-board CLI smokes. Review: independent.

## H1b Async path, events, group buffers and time

**Depends on** H1a-2. **Architecture:** "Session", "Time", "Device model", "Threading".

Scope:
- The asynchronous request path with the contract in the architecture: `Completion<T>`, a dispatcher per session, exactly-once completion through the dispatcher (never inline, never on the I/O thread), reply-order delivery, cancel without undo, `outcome_unknown` for sent mutations, close fails everything pending. The sync facade wraps it.
- Paged bulk transfers with a bounded number of chunks in flight (descriptors, capture reads, OTA writes; OTA keeps its retry exception).
- Events into the model: status stream, core events, diag events and Boot (which reloads the model) update it, and dispatched callbacks report model, status, diag, arm-state and link changes. Bounded queues with loss counters.
- Group data, pulled: a seqlock latest-value buffer per subscription group and a bounded sample ring per group (drop oldest, loss counter) for consumers that need every frame.
- Command groups: NaN-to-hold mapping and automatic prefix-only keepalives on the I/O thread.
- Time: periodic ping ownership (1 s, faster at first), the sliding-window fit restarted on Boot, and the mapped host time with uncertainty on every timestamped item. F8's `ClockEstimator`, ping API and CLI measurement are the base.
- The CLI keeps its output; mapped times may be added as new JSON fields.

Tests: the dispatcher contract (exactly once, order, cancel, close, timeouts), seqlock and ring stress tests with a reader that detects torn values, time mapping on synthetic clocks and across a Boot, 1 kHz groups on vdev. Consider a ThreadSanitizer run of the concurrency tests in host-linux.

Hardware check (bare board): `time_smoke.py` idle and traffic runs on the CH343 with mapped timestamps; a 1 kHz subscription read through the seqlock with no torn values; the CLI smokes. The Pi jitter row stays deferred.

Review: independent (concurrency, time).

## H1c Device identity (core v9)

**Depends on** H1a-2; may run beside H1b (they share `libdrivelink`, so the second to merge rebases). **Proposal:** [todo-device-name.md](todo-device-name.md). **Architecture:** "Device identity".

Scope:
- `GET_INFO` gains the device id (the factory MAC, 6 bytes) and the name (length-prefixed UTF-8, up to 16 bytes, empty by default). Recheck the field layout against `docs/protocol/core.md`.
- A `SET_NAME` system op: applies live, sets `nvs_dirty`, SAVE persists it, REVERT restores the saved name, allowed while armed. Where the name is stored (its own key or the A/B config) is the step plan's choice, read against `persistence.cpp`.
- Core v9: `protocol/defs/`, generated codecs and round-trip tests, the firmware system family and storage, native tests, the HIL test app and `protocol.py` where they read `GET_INFO`, `docs/protocol/`.
- Host: the model's identity, a typed service, a CLI `name` command; vdev with a stable fake id.

Hardware check (bare board): id and name read, rename, SAVE, reboot and readback, REVERT, rename while armed with mocks, the protocol, core and CLI smokes, stack marks.

Review: independent (wire and persistence).

## H2a GUI foundation

**Depends on** H1b, H1c. **Architecture:** "`drivelink-gui`", "Development environment". **Design:** [gui-design.md](gui-design.md) Decisions (G2, G3), [gui-design-status.md](gui-design-status.md).

Scope:
- `host/gui/` in the superbuild (`windows-mingw` and `linux` presets; `linux-no-gui` without it), `COLCON_IGNORE`. host-windows CI installs Qt 6.12.0; host-linux installs Ubuntu's Qt 6 packages through `tools/setup_linux.sh`, which this step introduces with the full current host-linux package list (`ci.yml` keeps none).
- **G8, folded in:** export the Figma variables to `host/gui/theme/tokens.json`, and a script that writes the `Theme` singleton (dark and light). Record in the GUI design files which open handoff issues each screen step settles (the G5 1080p issues go to H2d and H2e; the faults badge to H2b). If cheap, apply LP's placement enum to the Figma frames in the same pass.
- The DriveLink style on Basic: the controls the shell needs (Button, IconButton, ArmButton, StateChip, Badge, DeviceChip, NavRail, NavItem, StatusBar, Banner, Dialog, Toast, Tooltip); later steps add the rest. Inter, JetBrains Mono and the Lucide icons used, bundled.
- The shell: nav rail, the three-zone status bar, DISARM always reachable with Esc (also inside text fields, cancelling the edit), the faults drawer frame, the banner area, the modal scrim under the status bar, toasts.
- The backend: the connection interface (local now), Qt adapters over the device model through the session dispatcher, item-model bases, arm/disarm/acknowledge with refusal reasons, and the GUI's own command group (every commandable input at hold, keepalives) set up at connect.
- Test tooling: Qt Quick Test (offscreen) in both host checks; the backend tests against vdev; the scenario-screenshot tool (named screens to PNG at 2560×1440 and 1920×1080, dark and light, from vdev or a given endpoint); vdev scenario hooks (inject a fault, safe mode, a stalled link, a reboot, slow replies).

Not in scope: the screens' content (H2b onward). Connect is a minimal endpoint field until H2b.

Hardware check: vdev only.

Review: independent (DISARM and Esc, arming, command group keepalives, threading into Qt).

## H2b Connect and Device

**Depends on** H2a. **Design:** G3 frames and decisions, G7b Device loop table.

Scope:
- Connect: serial ports and baud, a TCP address for vdev, loading progress, protocol mismatch, recent devices keyed by device id (`QSettings`). The Agent tab is a disabled placeholder until H3b.
- Device: identity and versions with Rename; the loop table (rate, run time, budget computed on the host, overruns, links to the loop on Layout) and CPU per core; limits; ArmButton states with refusal reasons and Ack fault; the faults drawer with the device's names and descriptions, and the badge rule (decide active vs history, an open G8 issue); Save and Revert in the status bar with the Unsaved badge; reboot; factory reset; the safe-mode banner with EXIT_SAFE_MODE.
- OTA dialog: writing, waiting to confirm, rolled back. The GUI orchestrates through the library and keeps its rules: explicit REBOOT never repeated automatically, CONFIRM only after the full identity check.
- Link lost overlay (retry, disconnect) and the reboot notice (reset reason, what was lost; the GUI remembers which params changed since the last save).

Hardware check (bare board, mock modules): the scenario screenshots against `serial://COM3`; connect, rename, SAVE, reboot and readback; arm and disarm; fault and acknowledge; OTA with an `ota-image` candidate through the GUI backend; restoration of the board.

Review: independent (OTA orchestration, arm and acknowledge flows).

## H2c Layout

**Depends on** H2b. **Design:** G4 and G7b frames and decisions; [todo-loop-affinity.md](archive/todo-loop-affinity.md).

Scope:
- The instance tree by loop: selectable loop headers (name, rate, Real-time badge, budget), module rows indented under their controller.
- Instance and loop detail: the param table with typed editors (units, ranges, enums, pending, dirty and locked markers), the Wiring card (`Ref` pickers; other-loop targets disabled with the reason), Sources rows (target picker, value, age, valid/stale/missing/not connected), live Signals, generic Action forms (including paused actions with blobs), the custom panel slot (empty until H2f).
- Add instance or loop (type, label, loop chosen by realtime compatibility, the device default preselected; new loops default to Any core); Remove disabled with the reason on `Referenced`; labels; counts against `limits.*` with Add disabled at the limit.
- Pending changes: the review list, Discard all, refused ops shown on their row and not staged.

Hardware check: vdev only.

Review: self, unless the diff touches arm or command paths.

## H2d Monitor

**Depends on** H2c. **Design:** G5 Monitor frames and decisions.

Scope:
- The plot item: scene-graph line strips from the library's sample rings, stacked plots with a shared time axis, one plot per unit, Add plot, pause and cursors.
- The signal browser with the link budget each add would cost; refusal over 100 % or over the group limit with a one-click lower rate. GUI-owned subscription groups.
- Manual command: CommandControl per input through the GUI's command group, the slider (±limit, starting at 25 % of the descriptor max, remembered per input), dead-man ramp to 0 on release, Latch.
- The G5 1080p issues for Monitor (fitted plot areas, the paused toolbar).

Hardware check: vdev only.

Review: independent (manual commands drive actuators: dead-man, Latch, keepalives).

## H2e Tune

**Depends on** H2d. **Design:** G5 Tune frames and decisions.

Scope:
- Controller picker; the gains card (log slider with ÷4–×4 window, % steps and keys, ticks for the saved value and visible runs, live sends at most every 50 ms and on release, Revert gains to saved).
- Test signal and capture setup; Run (disabled until armed), run in progress, run stopped by a disarm.
- Runs: list, overlay with fixed colours, pinned runs, A/B compare, gain history with restore; Auto re-run (on, but only after the first manual Run in the connection; 0.5 s quiet; cancels a run in progress).
- Run metrics and plant identification (first-order-plus-deadtime fit, suggested gains) as pure library math with unit tests.
- Session files (versioned JSON, base64 columns, keyed by device id), open and save; CSV export per run.
- The G5 1080p issues for Tune (the plot column, the A/B card).

Hardware check: vdev. Deferred row: tuning a real motor with the GUI (first-run procedure).

Review: independent (Run and Auto re-run start test signals that move motors).

## H2f IMU calibration panel

**Depends on** H2c; may run beside H2d or H2e (it touches the panel slot and its own module directory). **Design:** G7 frames and decisions; `docs/protocol/imu.md`.

Scope:
- The custom panel mechanism: `host/gui/modules/<type>/`, matched by type name, compiled into the resources.
- The `imu.bno055` panel: live levels, orientation preview, Read and Export, Import review, Apply with confirmation, stale and unavailable states.
- Reconcile the G7 design with the built F6 (persistence through SAVE, the action and blob contract); define the PC calibration file format (versioned JSON, provenance, Euler convention).
- vdev runs it on `imu.mock`.

Hardware check: vdev. Deferred row: calibrating a real BNO055 with the GUI.

Review: self.

## H2g Installer and local GUI close-out

**Depends on** H2e, H2f.

Scope:
- An Inno Setup installer built by host-windows CI from a `windeployqt` staging directory (GUI, with the CLI and vdev), uploaded as an artifact. Unsigned.
- The root `VERSION` file (semver) read by the firmware's `PROJECT_VER`, the CMake projects, the `package.xml` files and the installer, with a `validate.py` check that they match; dev builds add the `git describe` suffix.
- Third-party notices for everything the installer bundles (Qt under LGPLv3 with its relinking terms, Inter, JetBrains Mono, Lucide, Asio, nlohmann JSON, and anything added since), shown in the installer and the GUI's About.
- Any remaining responsive or theme issues from the screen steps.
- `host/README.md` GUI usage; the GUI design files record what was built.

Hardware check (bare board, mock modules): the installed GUI across every screen against `serial://COM3`, with scenario screenshots; restoration.

Review: self.

## H3a Agent

**Depends on** H1b; planned after H2g by the build order. **Architecture:** "`drivelink-agent`".

Scope:
- `host/agent/` (`drivelink-agent`, Linux only, `COLCON_IGNORE`): serves several devices, each with its own session, I/O, model, lease, groups, clock model and history; reconnects a lost port without affecting the others; keeps running without clients; config file (bind, token, device list with aliases) plus flags.
- Access: `127.0.0.1` by default, LAN opt-in; the hello with agent protocol version, token and the device (alias or device id), refused before any device frame. One TCP connection per device; a hello without a device opens a lobby that only lists the devices.
- Agent MSG_TYPEs in `registry.yaml` from free command, response and event values (hello, device list, lease request/grant/revoke, clock model, client list, history), generated code, `docs/protocol/agent.md`.
- Arbitration: `seq` rewriting; group virtualization with id and channel rewriting and epochs; status, core and diag events to everyone.
- Lease: one holder for ARM, command groups and command frames; DISARM from any client; fast-path DISARM on a lease change or the holder's disconnect; lease-revoked message; exclusive operations (capture, testgen, actions, SAVE, REVERT, FACTORY_RESET, SET_NAME, layout ops, OTA) need the lease or a disarmed device with no holder; live param writes open to all.
- Time: the agent alone pings and distributes its clock model.
- History: bounded diag events and arm-state changes, sent on connect.
- Library: the agent client (hello with token and device on `TcpTransport`, the device list, lease API and events, the clock model in place of pings); CLI `--token`, `--device`, `agent-devices` and lease commands.
- The `agent` validate check (agent ↔ vdev in WSL), the agent CI job; `setup_linux.sh` gains what the agent needs.

Tests: unit tests for rewriting, epochs, the lease state machine, device selection and access; integration with two CLI clients, the agent and two vdevs (lease handoff per device, DISARM from any, revocation on disconnect, exclusive-op refusals, clock model, one vdev stopped while the other keeps working).

Hardware check: vdev only (the bench board through WSL comes in H3b).

Review: independent (arbitration, lease, safety, access).

Natural cut if large: H3a-1 server, devices, access, routing, events and agent MSG_TYPEs; H3a-2 lease, exclusive operations, clock model and history.

## H3b GUI remote mode

**Depends on** H3a, H2g. **Design:** G6 frames and decisions.

Scope:
- The remote connection over the library's agent client: observer first, command group only after a lease grant.
- Connect's Agent tab: typed address and token, recent agents, mDNS browse, then the agent's device list (alias, id, name, online state, lease holder) to pick a device. The device list is not in the G6 frames; design it in code from the G2 components and record it in the GUI design files. The agent advertises over mDNS/DNS-SD when LAN listening is on (a small pinned mDNS library, chosen here).
- The lease chip and the Clients and lease card; take, release, requesting, granted, revoked; the confirm-live-edit dialog with the per-connection opt-out; the ROS-driving banner; agent history in the faults drawer; unreachable and version-mismatch states; calibration actions need the lease.
- `tools/wsl_serial.py attach|detach|status` with `--dry-run`: guarded usbipd attach of the CH343 to WSL that always returns it to Windows.

Hardware check (PC, run by the session after the user's one-time `usbipd bind`): the agent in WSL owns the bench board through `wsl_serial.py`; the GUI on Windows connects over TCP; a lease handoff with a CLI as the second client; DISARM from the observer; the port returned to Windows and the board restored.

Review: independent (lease and arming in the GUI, the remote command group).

## H3c Pi deployment

**Depends on** H3a; may run beside H3b.

Scope:
- `host/agent/deploy/`: an install script that builds from source on the Pi and installs the binary, a systemd unit, udev rules giving each ESP's port a stable name, and the config file (bind address, token, device list); uninstall.
- A Pi setup guide: Ubuntu 26.04, the GPIO UART at 921600 with the serial console off, wiring (from the F8 row).
- Dry-run the script where possible (WSL with systemd, or a container).

Hardware check: deferred rows for the agent on the Pi and the remote GUI against it; the F8 Pi time row can run through it.

Review: self.

## H4a `drivelink_hardware`

**Depends on** H3a. **Architecture:** "`drivelink_hardware` (ROS 2)".

Scope:
- The ros2_control `SystemInterface` as the architecture describes: parameters, configure and activate, the lease, groups, the command mode switch with fresh hold values, ARM on the first claimed interface and DISARM on the last, `noexcept` `read()` / `write()` from the seqlock buffers and the command queue, NaN to hold, timestamps mapped to ROS time, `ERROR` on a disarm.
- Both connection modes: the agent (address, token and device alias or id), or a serial port directly. One hardware component per device.
- `setup_linux.sh` adds ROS Lyrical, ros2_control and rosdep (skipping what is installed); the `ros` validate check in WSL; the ros CI job in `ros:lyrical`.

Tests: plugin unit tests; integration against vdev through the agent and directly over a PTY.

Hardware check: vdev. Deferred row: the robot (first-run procedure).

Review: independent (real-time paths, arming, the lease).

## H4b `drivelink_bringup`

**Depends on** H4a.

Scope:
- An example two-wheel diff-drive robot with an IMU: URDF/xacro snippet, controllers YAML (`diff_drive_controller`, `joint_state_broadcaster`, `imu_sensor_broadcaster`), launch files.
- The diagnostics node: `/diagnostics` from the status stream and diag events, and a fault-acknowledge service.
- A launch test against vdev and the agent in the `ros` check; the documented recovery procedure.

Hardware check: vdev. Deferred row: the robot.

Review: self.

## H5 Agent package

**Depends on** H3c.

Scope: an arm64 `.deb` for the agent (binary, systemd unit, udev rules, config as a conffile), versioned from `VERSION`, built in CI, installed, upgraded and removed in a container; the install guide offers it beside the script.

Hardware check: deferred (installing on the Pi).

Review: self.

## H6 Close-out

**Depends on** every H step. Like F10:

- Remove stale text, archive finished step plans, bring `host/CLAUDE.md`, the READMEs and `docs/` in line with the code.
- Remove the temporary "Host overhaul in progress" section of the root `CLAUDE.md` and the overhaul workflow that only served it, as agreed with the user then.
- Bare board: the full smoke set and the installed GUI.
- After merge: merge `host_overhaul` into `main` and tag `vX.Y.Z` from `VERSION`. Whether open HW rows must pass first is decided with the user at that point.

Review: self.

## HW Hardware campaign

Unchanged: it runs every open row of "Deferred hardware checks" in [host-status.md](host-status.md) whenever hardware arrives, pausing the step in progress at a clean point. Host steps add rows for the GUI with a real motor (H2e) and a real IMU (H2f), the Pi (H3c, H5) and the robot (H4a, H4b).
