# Host overhaul: finished steps

The steps T0 to H3b-2, moved here from [host-steps.md](../host-steps.md) and
[host-status.md](../host-status.md) on 2026-11-20 once they were merged. The
live files keep the rules and the remaining steps. Each step's own plan and
results are the `h*-implementation.md` files in this folder; hardware results
are in [hardware-log.md](../hardware-log.md).

## Step table

| Step | Content | Hardware check | Review effort | State | PR |
|---|---|---|---|---|---|
| T0 | Tooling: C++23 for the host and native tests, MSYS2 UCRT64 GCC 16.2 for the native tests, native-tests CI in `ubuntu:26.04` | no | high, self | merged | [#68](https://github.com/taruntom1/drivelink_v2/pull/68) |
| H1a-1 | Typed values/descriptors, errors and synchronous services; CLI formatting with unchanged output. Depends on T0 | bare board: all planned CLI/mock/OTA smokes passed | high, independent | merged | [#69](https://github.com/taruntom1/drivelink_v2/pull/69) |
| H1a-2 | Device snapshot and host-windows CI. Depends on H1a-1 | bare board: snapshot/pending and CLI/mock smokes passed | high, independent | merged | [#70](https://github.com/taruntom1/drivelink_v2/pull/70) |
| H1b | Async path, events into the model, seqlock and sample-ring group buffers, command keepalives, periodic time and timestamp mapping. Depends on H1a-2 | bare board: time, 1 kHz groups, smokes | high, independent | merged | [#71](https://github.com/taruntom1/drivelink_v2/pull/71) |
| H1c | Device identity, core v9 (firmware, host, vdev, docs). Depends on H1a-2; may run beside H1b | bare board: identity, name persistence, smokes | high, independent | merged | [#72](https://github.com/taruntom1/drivelink_v2/pull/72) |
| H2a-1 | GUI build and style: `setup_linux.sh` and CI, Theme from Figma tokens (G8 done), fonts/icons, 13 shell controls, style gallery, Qt Quick Test. Depends on H1b, H1c | none (no device code); gallery checked against Figma | high, self | merged | [#73](https://github.com/taruntom1/drivelink_v2/pull/73) |
| H2a-2 | GUI shell and backend: app, shell, DISARM/Esc, scrim, Qt adapters, arm/ack, command groups, item-model bases, vdev scenario hooks, screenshot tool. Depends on H2a-1 | vdev: backend and ten-scenario campaign passed | high, independent | merged | [#74](https://github.com/taruntom1/drivelink_v2/pull/74) |
| H2b-1 | Connect and Device: ports, recents, mismatch, identity/Rename, loops/run time, faults drawer and badge, SAVE/REVERT/reboot/factory reset, safe-mode exit, link lost, reboot notice. Depends on H2a-2 | bare board: screenshots and `benchBoard` on COM3 passed | high, independent | merged | [#76](https://github.com/taruntom1/drivelink_v2/pull/76) |
| H2b-2 | OTA in the GUI: OTA workflow into libdrivelink (CLI unchanged), OTA dialog, firmware card. Depends on H2b-1 | bare board: `benchOta` installed and confirmed an `ota-image` candidate, then restored the exact original, on COM3 | high, independent | merged | [#77](https://github.com/taruntom1/drivelink_v2/pull/77) |
| H2c-1 | Layout structure and configuration: tree, loop/instance detail, typed param editors, wiring and source targets, Add instance/loop, Remove, labels, limits, pending review, Discard all, `refreshConfigAsync`. Depends on H2b-2 | vdev: backend Layout tests and the 23-scenario screenshot campaign passed | high, self | merged | [#78](https://github.com/taruntom1/drivelink_v2/pull/78) |
| H2c-2 | Layout live data and actions: GUI-owned subscription for the selected node (Signals, source value/age/state, loop Statistics), Actions card (paused, blobs), custom panel slot. Depends on H2c-1 | vdev: backend live tests and the 26-scenario screenshot campaign passed | high, independent | merged | [#79](https://github.com/taruntom1/drivelink_v2/pull/79) |
| H2d-1 | Monitor plots: signal browser with link budget and refusals, GUI-owned subscription groups on free slots, `PlotItem`, stacked plots, pause/cursors/readout, Snapshot CSV, Layout "Plot in Monitor"; Layout live group follows the page. Depends on H2c-2 | vdev: Monitor tests and screenshots; bare board: `benchMonitor` at 41/81/82 % of the link, 1000 Hz measured, 0 lost | high, independent | merged | [#80](https://github.com/taruntom1/drivelink_v2/pull/80) |
| H2d-2 | Monitor manual command: CommandControl per input, the hold rule (value and keepalive-only groups), slider, dead-man, Latch, zero around live writes. Depends on H2d-1 | vdev: command tests, screenshots, `benchCommand` dry run; deferred: motor | high, independent | merged | [#81](https://github.com/taruntom1/drivelink_v2/pull/81) |
| H2e-1 | Tune runs: core v10 (`ItemFlag::Tune`, u16 item flags), step metrics in libdrivelink, Tune page (controller and gains, test signal and capture, Run/Stop, run return to 0, runs list, overlay, pinning, metrics, 1080p plot column). Depends on H2d-2 | vdev: Tune tests, screenshots; bare board: v10 smokes, `benchBoard`, `benchTune` with a mock; deferred: motor | high, independent | merged | [#82](https://github.com/taruntom1/drivelink_v2/pull/82) |
| H2e-2 | Tune session: gain history and restore, A/B compare, Auto re-run (a gain change during a run cancels it), session files, CSV export, autosave and recovery. Depends on H2e-1 | vdev: Tune tests, screenshots, `benchTune` Auto re-run dry run; deferred: motor | high, independent | merged | [#83](https://github.com/taruntom1/drivelink_v2/pull/83) |
| H2e-3 | Tune plant identification: FOPDT fit and SIMC suggestions in libdrivelink, the tab and three cards, the mode switched for the step and written back, CheckItem warning. Depends on H2e-2 | vdev: identification tests, screenshots, `benchIdentify` dry run; deferred: motor | high, independent | merged | [#84](https://github.com/taruntom1/drivelink_v2/pull/84) |
| H2f | IMU calibration custom panel: `gui/modules/<type>/` mechanism, the `imu.bno055` panel (also `imu.mock`), versioned calibration file, generic backend additions, two style controls. Depends on H2c-2 | vdev: panel tests, screenshots, `benchCalibration` dry run; deferred: IMU | high, self | merged | [#85](https://github.com/taruntom1/drivelink_v2/pull/85) |
| H2g | Root `VERSION` and its check, Inno Setup installer (Release staging, `installer` check, CI artifact), third-party notices, Settings page (theme, About), close-out screenshot pass; added on review: Linux `.deb` (`deb` check, clean-container install job) CI split into test and package jobs, the firmware package and the tag release. Depends on H2e-3, H2f | bare board: the installed GUI on every screen against COM3, restored | high, self | merged | [#87](https://github.com/taruntom1/drivelink_v2/pull/87) |
| H3a-1 | Agent: server, access (Hello, token, lobby), devices, routing, subscription groups, events, agent MSG_TYPEs and Detail values; library agent client; CLI agent options; agent check and CI. Interim: no ARM or command groups through it. Depends on H1b (built after H2g) | vdev: 16 integration scenarios, unit tests | high, independent | merged | [#88](https://github.com/taruntom1/drivelink_v2/pull/88) |
| H3a-2 | Agent lease (Take preempts), exclusive operations, clock model (agent pings, composed clock), history, client list; agent protocol v2; CLI `--lease`, `agent-clients`. Depends on H3a-1 | vdev: 27 integration scenarios, unit tests | high, independent | merged | [#89](https://github.com/taruntom1/drivelink_v2/pull/89) |
| H3b-1 | GUI remote mode (Agent tab and device list, lease chip, take/release/revoked, held live edits, ROS banner, agent history, exclusive-operation gating); agent protocol v3 (client kind); `wsl_serial.py`. Depends on H3a-2, H2g | PC: `benchRemote` through the agent in WSL on the bench board passed (CLI takeover, observer DISARM, REBOOT and the agent's Boot path) | high, independent | merged | [#90](https://github.com/taruntom1/drivelink_v2/pull/90) |
| H3b-2 | mDNS: mjansson/mdns, agent responder (LAN binds only), library browse, CLI `agent-browse`, the Agent tab's "Found on the network" list, Settings > Connection. Depends on H3b-1 | PC (mirrored WSL): agent found in WSL and LAN probes answered; the same-PC GUI cannot reach mirrored WSL; deferred: a GUI on another machine (Pi) | high, self | merged | [#91](https://github.com/taruntom1/drivelink_v2/pull/91) |

## Overview rows

| Step | Content | Depends on | Hardware check | Review |
|---|---|---|---|---|
| T0 | Tooling: C++23 for the host and native tests, MSYS2 UCRT64 GCC 16.2 for the native tests, native-tests CI in `ubuntu:26.04` | — | no | self |
| H1a-1 | Typed library: types, services and errors, CLI on the typed API | T0 | bare board: CLI smokes | independent (large diff) |
| H1a-2 | Device model snapshot and host-windows CI | H1a-1 | bare board: CLI smokes | independent |
| H1b | Async path, events into the model, group buffers, command keepalives, periodic time and timestamp mapping | H1a-2 | bare board: time, 1 kHz groups, smokes | independent |
| H1c | Device identity, core v9 (firmware, host, vdev, docs) | H1a-2; may run beside H1b | bare board: identity, name persistence, smokes | independent |
| H2a-1 | GUI build and style: `setup_linux.sh` and CI, Theme from Figma tokens (G8 folded in), fonts and icons, the shell controls, style gallery, Qt Quick Test | H1b, H1c | none (no device code) | self |
| H2a-2 | GUI shell and backend: app, shell, DISARM/Esc, Qt adapters, arm/ack, command groups, item-model bases, vdev scenario hooks, screenshot tool | H2a-1 | vdev | independent |
| H2b | Connect and Device screens (H2b-1), OTA (H2b-2) | H2a-2 | bare board: GUI scenarios | independent |
| H2c | Layout screen | H2b | vdev | self |
| H2d | Monitor screen: plots (H2d-1), manual command (H2d-2) | H2c | vdev; H2d-1 also a short bare-board run | independent |
| H2e | Tune screen: runs and metrics (H2e-1), session (H2e-2), plant identification (H2e-3) | H2d | vdev; H2e-1 also bare board (wire change); deferred: motor | independent |
| H2f | IMU calibration custom panel | H2c; may run beside H2d/H2e | vdev; deferred: IMU | self |
| H2g | Inno Setup installer (unsigned), root `VERSION`, third-party notices, local GUI close-out | H2e, H2f | bare board: installed GUI | self |
| H3a-1 | Agent for several devices: server, access, routing, subscription groups, events, agent MSG_TYPEs; library agent client; agent check and CI. Interim: nobody can arm through it | H1b (planned after H2g) | vdev | independent |
| H3a-2 | Agent lease (Take preempts), exclusive operations, clock model, history, client list; CLI lease commands | H3a-1 | vdev | independent |
| H3b | GUI remote mode with the agent's device list, mDNS, `wsl_serial.py` | H3a, H2g | PC: agent in WSL owns the bench board | independent |

## Items settled by the step plans

| Item | Step |
|---|---|
| Where the device name is stored (its own NVS key or the A/B config); allowed characters; vdev's stable fake id | H1c |
| Faults badge: active faults only, or history too | H2b |
| What the reboot notice lists as lost (the GUI tracks params changed since the last save) | H2b |
| G5 1080p layouts: A/B card, gain history placement (the Tune plot column settled in H2e-1, Monitor's in H2d-1) | settled in H2e-2 |
| How Open/Save pick a session file (a native dialog cannot pass Esc to DISARM) | settled in H2e-2: non-modal native dialogs, allowed while armed (user decision) |
| How suggested gains map onto tune-flagged params generically (no module names) | settled in H2e-3: naming conventions in `docs/adding-a-module.md` (`<prefix>.kp/.ki/.kd`, responses named after the prefix, the `open_loop` value); no wire change (user decision) |
| Calibration file format, provenance, Euler convention, cancel and reconnect behaviour | settled in H2f: versioned JSON of the decoded fields with provenance (`host/gui/modules/imu.bno055/README.md`); Euler as the device reports it, none in the file; nothing re-sent, interruptions recorded as "may have run" |
| Agent default port, history sizes, retry policy for an offline device | port 47620 and a 2 s fixed retry settled in H3a-1; history 256 entries per device (`history_size`, 16..4096) settled in H3a-2 |
| mDNS library, service name and TXT records | settled in H3b-2: mjansson/mdns 1.4.3, `_drivelink._tcp`, TXT `txtvers`/`proto`/`version` (user decisions; `docs/protocol/agent.md` "Discovery") |
| Where the GUI keeps agent tokens (`QSettings` or Windows Credential Manager) | settled in H3b-1: the GUI's INI file, only when "Remember token" is checked (user decision) |
| The device-list screen on Connect's Agent tab (not in Figma) | settled in H3b-1: built from the G2 components, recorded in `gui-design.md` |

## Model and review exceptions

The user approved these per step, on top of "Models and effort" in the status file.

- **T0 exception (user-approved 2026-11-14).** This Codex session plans and builds T0; self-review at high effort is sufficient for its tooling-only diff. The user approved the T0 plan before edits.

- **H1a exception (user-approved 2026-11-15).** Codex plans/builds H1a, with an independent Codex review at high effort. The user also approved the H1a-1/H1a-2 split; both parts precede H1b/H1c.

- **H1b exception (user-approved 2026-11-15).** Codex plans/builds H1b with an
  independent Codex review at high effort. The user kept H1b in one PR after a
  proposed split. The approved scope and runtime choices are in
  [h1b-implementation.md](h1b-implementation.md).

- **H1c exception (user-approved 2026-11-15).** Codex plans/builds H1c with an independent Codex review at high effort. The user approved the plan and leaving verified normal core v9 installed; see [H1c plan/results](h1c-implementation.md).

- **H2f exception (user-approved 2026-11-18).** Opus 5.5 (high) plans and builds the backend additions, the panel logic (typed C++ `calibration.*` and its QML singleton, `CalibrationPanel.qml`; ported from JavaScript at the user's request after the first review), the tests and fixes; a Sonnet 5.5 subagent (high) builds the drawing-only cards, the Apply dialog, `DLCalibrationLevel`, `DLOrientationPreview`, their gallery entries and control tests, and the two screenshot scenarios from the panel's written contract, reviewed by the session. Self-review (the brief's column).

- **H3b-2 exception (user-approved 2026-11-20).** Opus 5.5 (high) plans, writes the contracts (`discovery.hpp`, agent.md "Discovery", the QML property contract), the library browse and parsing, the agent responder, the GUI backend, integrates, fixes and reviews; Sonnet 5.5 subagents (high) built from written contracts: the agent config keys and flags with tests and the mdns notice; CLI `agent-browse` and the integration discovery scenarios; the Agent tab's found list, Settings' Connection card, `AppSettings::discoverAgents`, their tests and screenshot scenarios. Self-review (the brief's column).

- **H3b-1 exception (user-approved 2026-11-19).** Opus 5.5 (high) plans, writes the wire contract (agent v3), the backend and its pure rules, the Shell, Connect and Device wiring, integrates, fixes and reviews; Sonnet 5.5 subagents (high) built `tools/wsl_serial.py` with its tests, the style controls (`DLLeaseChip`, the status bar's lease slot, the dialog's confirmation box) with gallery entries and tests, the app's remote QML components, and the remote screenshot scenarios, from written contracts reviewed by the session. Independent review on Opus (high), as required.

- **H3a-2 exception (user-approved 2026-11-19).** Opus 5.5 (high) plans, writes the contracts (`agent.md` v2, the registry, `agent.hpp`), the lease and group logic with their tests, the agent wiring, the Session clock and agent ops, integrates, fixes and reviews; Sonnet 5.5 subagents (high) did the read-only planning research and built from the contracts: the codec and its tests, then the CLI changes and the integration scenarios. Independent review on Opus (high), as required.

- **H3a-1 exception (user-approved 2026-11-19).** Opus 5.5 (high) plans, writes the contracts (`agent.md`, the registry entries, `agent.hpp`, `config.hpp`), the Session changes, the agent core and group map with their tests, integrates, fixes and reviews; Sonnet 5.5 subagents (high) build from those contracts: the agent codec and its tests, the agent config and its tests, the tooling (`agent` check, `setup_linux.sh agent`, CI job, development.md rows), the CLI options and the integration script, and after review integration scenarios 10–16. Independent review on Opus (high), as required.

- **H2g exception (user-approved 2026-11-19).** Opus 5.5 (high) plans and builds the version plumbing, the firmware guard, `AppSettings`, `validate.py`/CI, the installed-GUI script, the docs and the review; Sonnet 5.5 subagents (high) build `package_win.py` with its tests, the Inno Setup script and the notices (manifest, licence texts, generator), and the Settings page QML with its shell tests and scenarios, from written contracts, reviewed by the session. Self-review (the brief's column).

- **H2e-3 exception (user-approved 2026-11-18).** Opus 5.5 (high) plans and builds the backend, tests and fixes; a Sonnet 5.5 subagent (high) builds the QML cards, the CheckItem Warning state and the screenshot option from a written contract, reviewed by the session. Independent review on Opus (high) as required.

- **H2a-2 exception (user-approved 2026-11-15).** Codex plans/builds H2a-2 with independent Codex review at high effort. The user approved the plan and keepalive-only/already-armed observation choices; [plan/results](h2a-2-implementation.md). Review and vdev checks completed 2026-11-16.


## Decision log rows

Rows whose decisions are now stated in the plans, the docs, the READMEs or
`firmware/test/CLAUDE.md`.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-20 | H3b-2 | User: TXT `txtvers`, `proto`, `version` only (no device data); Settings > Connection "Find agents on the network" (on by default); the PC check with WSL in mirrored networking. Internal: IPv4 only; the agent's own target host `drivelink-<hostname>.local.`; no conflict probing; the responder on its own thread, one socket per interface, a 10 s re-scan; multicast answers at most once a second per record set and interface; legacy unicast echoes id and question (TTL at most 10 s); browse = one QU query per interface (again at 300 ms), the endpoint from the sender when it is an A record; GUI rounds back off 1 s to 60 s and drop an agent missed twice; picking a found agent clears a token typed for another address; CLI `agent-browse` (`--timeout-ms`, default 1500); config `advertise`/`name`, flags `--no-advertise`/`--name`; an mDNS setup failure is logged and TCP continues. Same-PC Windows to mirrored-WSL discovery proved impossible, so the GUI check is a deferred Pi row | H3c and HW run the deferred row; discovery stays outside the TCP protocol | yes (agent.md, READMEs, gui-design, bench.md, host-steps) |
| 2026-11-13 | F6, F10 | Test harnesses retry group-only Busy/StagingBusy rejections (in addition to Withdrawn), bounded to ten attempts; production SDK/CLI stays unchanged. F10 applies the same bounded retry to the native "autonomous loops" group test (`test_core_groups.cpp`) | CI's existing sub-enable scenario raced a loop's atomic slot claim (host-linux in F6; the native test on PR #60's CI, where the owning loop's `endTick` held the slot during `enable`). The rejection occurs before mutation; unit tests and independent review confirm timeouts/actions/expected refusals are not broadened | n/a |

## Briefs

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

Hardware check (bare board): the CLI smokes that cover the moved services: `hardware_smoke.py`, motor, IMU, capture and testgen with mocks, loop affinity, and `ota_smoke.py` with an `ota-image` candidate. Restore the board to the "Board state" in [host-status.md](../host-status.md).

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

User-approved split (2026-11-15): H2a-1 builds the style layer below; H2a-2
builds the shell and backend. [H2a-1 plan and results](h2a-1-implementation.md).

- **H2a-1** (self-review, no hardware): `host/gui/` in the superbuild, `setup_linux.sh` and CI, G8 tokens and the `Theme` generator, bundled fonts/icons, the shell controls (Button … Tooltip), a style gallery, Qt Quick Test in both host checks. Done; see the plan.
- **H2a-2** (independent review, vdev): the application, the shell with Esc → DISARM, the scrim under the status bar, the faults drawer frame, toasts; the backend and item-model bases; vdev scenario hooks; the screenshot tool, grown from the gallery (`drivelink-style-gallery`).

H2a-2's [approved plan and results](h2a-2-implementation.md) settle the direct
connection behavior: prefix-only keepalives leave input values unchanged;
descriptor defaults are not semantic holds. An already-armed device is
observed until explicit DISARM acknowledgement, including watchdog expiry
during discovery. Command groups are partitioned by owning loop and capacity.

**Depends on** H1b, H1c. **Architecture:** "`drivelink-gui`", "Development environment". **Design:** [gui-design.md](../gui-design.md) Decisions (G2, G3), [gui-design-status.md](gui-design-status.md).

Scope:
- `host/gui/` in the superbuild (`windows-mingw` and `linux` presets; `linux-no-gui` without it), `COLCON_IGNORE`. host-windows CI installs Qt 6.12.0; host-linux installs Ubuntu's Qt 6 packages through `tools/setup_linux.sh`, which this step introduces with the full current host-linux package list (`ci.yml` keeps none).
- **G8, folded in:** export the Figma variables to `host/gui/theme/tokens.json`, and a script that writes the `Theme` singleton (dark and light). Record in the GUI design files which open handoff issues each screen step settles (the G5 1080p issues go to H2d and H2e; the faults badge to H2b). If cheap, apply LP's placement enum to the Figma frames in the same pass.
- The DriveLink style on Basic: the controls the shell needs (Button, IconButton, ArmButton, StateChip, Badge, DeviceChip, NavRail, NavItem, StatusBar, Banner, Dialog, Toast, Tooltip); later steps add the rest. Inter, JetBrains Mono and the Lucide icons used, bundled.
- The shell: nav rail, the three-zone status bar, DISARM always reachable with Esc (also inside text fields, cancelling the edit), the faults drawer frame, the banner area, the modal scrim under the status bar, toasts.
- The backend: the connection interface (local now), Qt adapters over the device model through the session dispatcher, item-model bases, arm/disarm/acknowledge with refusal reasons, and the GUI's own command groups (running discovered inputs by loop/capacity, prefix-only keepalives) set up on a disarmed direct connection; already-armed observation as above.
- Test tooling: Qt Quick Test (offscreen) in both host checks; the backend tests against vdev; the scenario-screenshot tool (named screens to PNG at 2560×1440 and 1920×1080, dark and light, from vdev or a given endpoint); vdev scenario hooks (inject a fault, safe mode, a stalled link, a reboot, slow replies).

Not in scope: the screens' content (H2b onward). Connect is a minimal endpoint field until H2b.

Hardware check: vdev only.

Review: independent (DISARM and Esc, arming, command group keepalives, threading into Qt).

## H2b Connect and Device

**Depends on** H2a-2. **Design:** G3 frames and decisions, G7b Device loop table.

User-approved split (2026-11-17): **H2b-1** builds everything below except OTA,
with libdrivelink serial-port listing, the active-only faults badge and loop
run time from GUI-owned free-slot subscriptions. **H2b-2** moves the CLI's OTA
reboot, identity check and CONFIRM workflow into libdrivelink (CLI output
unchanged), adds the OTA dialog and the firmware card's update action, and runs
the OTA bare-board check. Both are independently reviewed.

Scope:
- Connect: serial ports and baud, a TCP address for vdev, loading progress, protocol mismatch, recent devices keyed by device id (`QSettings`). The Agent tab is a disabled placeholder until H3b.
- Device: identity and versions with Rename; the loop table (rate, run time, budget computed on the host, overruns, links to the loop on Layout) and CPU per core; limits; ArmButton states with refusal reasons and Ack fault; the faults drawer with the device's names and descriptions, and the badge rule (decide active vs history, an open G8 issue); Save and Revert in the status bar with the Unsaved badge; reboot; factory reset; the safe-mode banner with EXIT_SAFE_MODE.
- OTA dialog: writing, waiting to confirm, rolled back. The GUI orchestrates through the library and keeps its rules: explicit REBOOT never repeated automatically, CONFIRM only after the full identity check.
- Link lost overlay (retry, disconnect) and the reboot notice (reset reason, what was lost; the GUI remembers which params changed since the last save).

Hardware check (bare board, mock modules): the scenario screenshots against `serial://COM3`; connect, rename, SAVE, reboot and readback; arm and disarm; fault and acknowledge; OTA with an `ota-image` candidate through the GUI backend; restoration of the board.

Review: independent (OTA orchestration, arm and acknowledge flows).

## H2c Layout

User-approved split (2026-11-17): **H2c-1** (self-review) builds the tree,
loop and instance detail, typed param editors, wiring and source targets,
Add instance/loop, Remove, labels, limits, the pending review, Discard all and
refused ops. **H2c-2** (independent review: GUI-owned subscription groups and
actions, including paused ones) adds live Signals, the source value/age/state
rows, loop Statistics, the Actions card and the custom panel slot. A typed
param value is sent on Enter or focus loss (user decision 2026-11-17).

**Depends on** H2b. **Design:** G4 and G7b frames and decisions; [todo-loop-affinity.md](todo-loop-affinity.md).

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

Split (user decision 2026-11-17), one session and PR each, both independently reviewed:
- **H2d-1 plots:** everything above except manual command; its card shows a placeholder. The Layout live group is released while Layout is not shown, so Monitor can use its slot; Monitor uses free slots only and refuses an add when none is free. Layout's Signals card gains "Plot in Monitor". Done: [h2d-implementation.md](h2d-implementation.md).
- **H2d-2 manual command**, with the **hold rule** (user decision 2026-11-17): only numeric inputs whose descriptor range is finite and includes 0 can be commanded. At connect each loop's inputs split into a value group (those inputs; hold 0, the value the firmware sets at ARM) and a keepalive-only group (inputs without such a range, such as `position_sp`) that never carries values. Every value sent is one shown on a slider, or 0. Non-commandable inputs are listed disabled with the reason. Before this GUI writes a live param to an instance it is commanding, it discards queued values. Done: [h2d-implementation.md](h2d-implementation.md) "H2d-2".

Hardware check: vdev; H2d-1 also ran a short bare-board check (1 kHz plotting near and below 100 % of the link, nothing flashed).

Review: independent (H2d-1: the shared subscription slots and the render-thread data path; H2d-2: manual commands drive actuators: dead-man, Latch, keepalives).

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

Split (user decision 2026-11-18), one PR each, each independently reviewed:
- **H2e-1 Runs:** core v10 item flags u16 with `ItemFlag::Tune` (gains are
  marked in the definitions, not by name), step metrics in libdrivelink, the
  Tune page: controller picker, gains card, test signal and capture, Run/Stop,
  run in progress and cut by a disarm, runs list, overlay, pinning, metrics,
  the 1080p plot column. Bare board: v10 smokes and `benchTune` with a mock.
- **H2e-2 Session:** gain history with restore, A/B compare and its 1080p card,
  Auto re-run, session files (versioned JSON, base64 columns, keyed by device
  id; how Open/Save pick a file), CSV export per run. Done:
  [h2e-implementation.md](h2e-implementation.md) "H2e-2" (user decisions
  2026-11-18: with Auto re-run on a gain change cancels the run, off the gains
  lock; Auto re-run needs the user's Run under each ARM; non-modal native file
  dialogs allowed while armed; Open never writes to the device and Restore
  matches by names; autosave with recovery on the next connect).
- **H2e-3 Plant identification:** first-order-plus-deadtime fit and suggested
  gains in libdrivelink, the identification tab, the CheckItem warning state;
  how suggested gains map onto tune-flagged params generically. Done (user
  decisions 2026-11-18: the step switches the mode to `open_loop` and writes
  it back; naming conventions, no wire change; SIMC PI with Robust /
  Balanced / Fast; a "from" pre-level, built as one square period of the
  generator): [h2e-implementation.md](h2e-implementation.md) "H2e-3".

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

**Split (user decision 2026-11-19):** H3a-1 (merged scope below without the lease, exclusive operations, clock model and history; [plan](h3a-1-implementation.md)) and H3a-2 (those; [plan](h3a-2-implementation.md)). Take preempts: fast-path DISARM, revoke, clear the old holder's command groups, grant. Agent refusals use shared Detail values 54..58; core stays v10. H3a-2 bumps the agent protocol to v2. User decisions in H3a-2 (2026-11-19): clients ping the agent and compose its clock model (valid on remote clients too); REBOOT and EXIT_SAFE_MODE are exclusive too; every lease end (release, Take, disconnect) DISARMs and clears the holder's command groups; the CLI gets `--lease` and `agent-clients`.

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

**Split (user decision 2026-11-19):** H3b-1 (everything below except mDNS;
[plan](h3b-1-implementation.md)) and H3b-2 (mDNS: the mjansson/mdns
library, the agent advertising when LAN listening is on, the library's browse
and the Agent tab's "found on the network" list, notices;
[plan](h3b-2-implementation.md)). User decisions in H3b-2: TXT
`txtvers`/`proto`/`version`, a Settings toggle for discovery, the PC check
with WSL mirrored (same-PC discovery proved impossible; the GUI check is a
deferred Pi row). User decisions in
H3b-1: agent tokens in the GUI's INI file only when "Remember token" is
checked; ROS is recognised by a client kind in the Hello (`AgentClientKind`:
Other, Gui, Cli, Ros), so agent protocol v3 carries `client_kind` in Hello,
`kind` in ClientList and `holder_kind` in the device record and the Lease
event.

Scope:
- The remote connection over the library's agent client: observer first, command group only after a lease grant.
- Connect's Agent tab: typed address and token, recent agents, mDNS browse, then the agent's device list (alias, id, name, online state, lease holder) to pick a device. The device list is not in the G6 frames; design it in code from the G2 components and record it in the GUI design files. The agent advertises over mDNS/DNS-SD when LAN listening is on (a small pinned mDNS library, chosen here).
- The lease chip and the Clients and lease card; take, release, requesting, granted, revoked; the confirm-live-edit dialog with the per-connection opt-out; the ROS-driving banner; agent history in the faults drawer; unreachable and version-mismatch states; calibration actions need the lease.
- `tools/wsl_serial.py attach|detach|status` with `--dry-run`: guarded usbipd attach of the CH343 to WSL that always returns it to Windows.

Hardware check (PC, run by the session after the user's one-time `usbipd bind`): the agent in WSL owns the bench board through `wsl_serial.py`; the GUI on Windows connects over TCP; a lease handoff with a CLI as the second client; DISARM from the observer; the port returned to Windows and the board restored.

Review: independent (lease and arming in the GUI, the remote command group).

