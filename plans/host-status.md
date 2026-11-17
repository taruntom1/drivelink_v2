# Host status

The working state of the host overhaul: the steps in [host-steps.md](host-steps.md), building [host-architecture.md](host-architecture.md) on the `host_overhaul` branch. **Every PR into `host_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

The firmware overhaul (S0, S1, F1–F10, LP) is finished; its step table, decision log and hardware results are archived in [archive/firmware-status.md](archive/firmware-status.md). Its design record is [firmware-architecture.md](firmware-architecture.md).

## Current

- **Host re-planned (2026-11-14).** [host-architecture.md](host-architecture.md) is the design record and [host-steps.md](host-steps.md) holds a brief per step and the items left to step plans. Order: library, GUI in local mode, agent and remote mode, ROS. This file replaced `overhaul-status.md` the same day. [PR #67](https://github.com/taruntom1/drivelink_v2/pull/67).
- **Next step:** H2d-2 (Monitor manual command) after H2d-1 merges, or H2f (IMU calibration panel). H2c-2 (#79) is merged; H2d-1 (Monitor plots) is built, independently reviewed, and checked on vdev and the bare board. HW runs as hardware arrives.
- **Branches.** `main` is frozen until `host_overhaul` merges (H6). `legacy-final` is on `7a4d693`. Every H step and HW fix branches from `origin/host_overhaul` and targets it.
- **No motors, IMU or Pi on the bench.** Checks that need them are deferred: each step writes its script now and adds a row to "Deferred hardware checks". No step waits for them.

## Keeping this file short

Every session reads all of this file, so it holds only what has no other home.

- **A fact goes where its subject lives.** If it is already there, don't repeat it here.

  | Subject | Home |
  |---|---|
  | What each step does, items left to step plans | `plans/host-steps.md` |
  | Design and its reasons | `plans/host-architecture.md`, `plans/firmware-architecture.md`, `plans/gui-design.md` |
  | Wire format, result and detail codes, diag codes | `docs/protocol/` |
  | Firmware and host rules, build and test commands | the scoped `CLAUDE.md` files, `docs/development.md`, `host/README.md` |
  | The bench board, ports, serial traps, pins, stack marks | `docs/bench.md` |
  | Hardware results | `plans/hardware-log.md` |
  | Firmware overhaul history | `plans/archive/firmware-status.md` |

- **The handoff is replaced, not appended.** Write what the next step must know. Move anything a later step needs to "Notes for later steps", under that step.
- **A note is deleted by the step that uses it.**
- **A decision log row stays while a later step needs it.** Once the plan, a doc or a README states it, the next session that edits this file removes the row.
- **Board state** is one paragraph, replaced by the step that changes the board.

## Handoff for H2d-2 / H2f / HW

- **Read first:** [host/gui/README.md](../host/gui/README.md) "Monitor (H2d-1)" and "Layout (H2c-1, H2c-2)", the GUI rules in [host/CLAUDE.md](../host/CLAUDE.md) (subscriptions, the H2d-2 hold rule) and [h2d-implementation.md](h2d-implementation.md) (user decisions, results). The H2d brief in [host-steps.md](host-steps.md) has the H2d-2 scope.
- **What exists:** `backend/monitor_model.*` (pure: catalogue, rates, link budget, plot set, plans, rings, decimation, axes, cursors, CSV), `backend/monitor_view.*` (`MonitorView`, QML `connection.monitor`), `backend/connection_monitor.cpp` (`setPage`, `syncMonitor`, `pollMonitor`: one GUI-owned subscription group per plotted loop), `app/plot_item.*` (`PlotItem`, a `QML_ELEMENT` in the static `DriveLink.Shell` module: call `qml_register_types_DriveLink_Shell()` in main and tests) and `app/MonitorPage.qml` with the manual command card as a placeholder. `tests/test_support.hpp` has the shared `Fixture`, `waitFor`, `visualChild`. The app has `--plot`, `--rate`, `--window` and the `monitor-paused` / `monitor-budget` screens.
- **H2d-2:** the command card replaces the placeholder in the Signals column. Command values go through the GUI's own command groups (`setupGroups` in `connection.cpp`, prefix-only keepalives today); the hold rule (user decision 2026-11-17, host rules and architecture) splits each loop's inputs at connect into a value group (finite range including 0, hold 0) and a keepalive-only group. Mutations: one at a time across command setup, loop statistics, the Layout live group and Monitor (each gates on the others' pending flags).
- **Traps:** only the Layout live group follows the page (`m_page`, default "layout"); Monitor's groups stay while other pages show. A lost link keeps Monitor's plot set (`MonitorView::lose()`); an explicit close or another device resets it. A subscription group carries at most 1 kHz (`kMaxGroupRateHz`: the firmware link task sends each group's latest sample once per 1 ms; faster samples overwrite and count as lost). Qt's software renderer draws no custom geometry: `PlotItem` paints an image there (tests, screenshots) and line strips on RHI. A QML `Repeater` over a JS list recreates its delegates; stateful rows need an in-place list model. The gui-backend test needs `-Wa,-mbig-obj` on MinGW.
- **Open edges:** no per-group "samples stopped" indicator (returning to Monitor re-reads LIST; a failed pass retries 3 times); `others` in the budget is unknown until Monitor's first LIST (Plot in Monitor opens the page, which reads it). Remaining from H2c/H2b: a failed live setup retries on the next selection; a paused action's staged params add no line to the change list; enum names as served; another client's param changes get no Unsaved marker; command groups configured before a stalled link stay until reboot; ACK can stay locally blocked if a Cleared event is missed while another fault is active; vdev reports reset reason 0.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further and add rows here (for example H3a-1 and H3a-2). S0, S1, F1–F10 and LP are merged; see the archive.

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
| H2d-1 | Monitor plots: signal browser with link budget and refusals, GUI-owned subscription groups on free slots, `PlotItem`, stacked plots, pause/cursors/readout, Snapshot CSV, Layout "Plot in Monitor"; Layout live group follows the page. Depends on H2c-2 | vdev: Monitor tests and screenshots; bare board: `benchMonitor` at 41/81/82 % of the link, 1000 Hz measured, 0 lost | high, independent | in review | [#80](https://github.com/taruntom1/drivelink_v2/pull/80) |
| H2d-2 | Monitor manual command: CommandControl per input, the hold rule (value and keepalive-only groups), slider, dead-man, Latch. Depends on H2d-1 | vdev | high, independent | planned | |
| H2e | Tune screen, run metrics, plant identification, session files. Depends on H2d | vdev; deferred: motor | high, independent | planned | |
| H2f | IMU calibration custom panel. Depends on H2c-2 | vdev; deferred: IMU | high, self | planned | |
| H2g | Inno Setup installer, local GUI close-out. Depends on H2e, H2f | bare board: installed GUI | high, self | planned | |
| H3a | Agent: access, arbitration, lease, clock model, history, agent MSG_TYPEs; library agent client; agent check and CI. Depends on H1b (built after H2g) | vdev | high, independent | planned | |
| H3b | GUI remote mode, mDNS, `wsl_serial.py`. Depends on H3a, H2g | PC: agent in WSL owns the bench board | high, independent | planned | |
| H3c | Pi deployment: install script, systemd unit, udev rule, config. Depends on H3a | deferred: Pi | high, self | planned | |
| H4a | `drivelink_hardware`, ROS in `setup_linux.sh`, ros check and CI. Depends on H3a | vdev; deferred: robot | high, independent | planned | |
| H4b | `drivelink_bringup`: example diff-drive + IMU, diagnostics node, launch test. Depends on H4a | vdev; deferred: robot | high, self | planned | |
| H5 | Agent `.deb` (arm64). Depends on H3c | deferred: Pi | high, self | planned | |
| H6 | Cleanup, docs, merge `host_overhaul` into `main`. Depends on every H step | bare board: full smoke and GUI | high, self | planned | |
| HW | Hardware campaign on `host_overhaul`: run every open row of "Deferred hardware checks"; firmware fixes go to `host_overhaul` as PRs | yes: motors, IMU, Pi, robot | high | planned | |

## Notes for later steps

Each note is deleted by the step that uses it.

- **H2f.** The custom panel slot loads `DriveLink/Modules/<type name>/Panel.qml` from the resources with `backend` and `nodeKey` (an optional `title` names the tab). Actions go through `backend.checkAction` / `backend.runAction` (sent once; blobs as hex, at most 512 bytes); the selection's live values are in `backend.live`. Calibration files and their dialog belong to the panel (user decision 2026-11-17: no new native dialog in Layout).
- **H2b–H3b.** Each settles the GUI design issues assigned to it in [gui-design.md](gui-design.md), "Open issues by host step", and records the outcome there.

## Deferred hardware checks

A check that needs motors, a real IMU, the Pi or the robot doesn't block its step:

- **The step writes the script now** (under `firmware/test/uart_comm/` or `host/test/`) and dry-runs it against vdev or the bare board with mock modules, so the script is known to work. It runs with real modules through a flag, not a second script.
- **It adds a row here:** the step, the script and its command, the hardware it needs, what it checks and what result means pass.
- **When hardware arrives,** the step in progress pauses at a clean point and the rows for that hardware run before any further step. Otherwise they run in HW.
- **A motor script starts with the first-run procedure:** wheels off the ground, a current-limited supply, a low duty cap and a hand on the power switch.
- **A row is deleted** once it has passed, with a line in [hardware-log.md](hardware-log.md).

| Step | Script and command | Needs | Checks | Pass |
|---|---|---|---|---|
| F5a-1 / F5a-2 / F5b | `python host/test/motor_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --confirm-first-run --pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G] [--duty-cap 0.2] [gains/targets/tolerances]`, normal firmware; then the same with `--stall` on a `CONFIG_CORE_BENCH_CRASH=y` build (flash the normal firmware afterwards). Both passed on the bare board with `--no-motor` (`--pool-check` on the normal build) | Motor driver, motor and encoder | First-run procedure; encoder polarity in open loop before any closed loop; velocity and position tracking; watchdog force; longest tick; with `--stall`, a 2 s control-loop stall in velocity mode | Polarity positive; tracking within the given tolerances; after the watchdog trip `forced` is set, applied duty 0 and the encoder stops (at most 2 counts in 0.5 s); zero overruns in the normal run (a stall run counts its stall as overruns); stall: Fault/LoopStalled during the stall, the encoder moves at most 25% of its free run at the pre-stall speed, `forced` and zero duty after it |
| F6 | `python host/test/imu_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --sda 8 --scl 9 --address 0x28 --calibration-wait 300` (normal firmware; add `--stack-wait 45` for stack observation). Mock workflow and real `--missing-sensor` both passed on the bare board | BNO055, suitable I2C wiring/pull-ups | Signal groups and stationary plausibility; operator reaches levels 3/3/3/3; exact profile apply and boot restore in CONFIG; NDOF recovery; layout/settings restoration | Valid fresh samples with plausible quaternion/gravity/temperature; 3/3/3/3 before profile capture; exact CONFIG profile readback and all staged params survive SAVE/reboot; modes/cleanup pass; all task stacks >1 KB. Physical orientation/calibration quality must also be checked on the sensor |
| F7b | `python host/test/testgen_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --confirm-first-run --pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G --duty-cap 0.2 --probe-duty 0.1 --min-response R] --loop-rate 1000` (normal firmware). Same workflow passed with mocks on vdev and bare board | Motor driver, motor and encoder | First-run procedure; positive step before chirp, generated input/response at 1 kHz, STOP/hold/fresh handoff and watchdog cancellation | Positive response above the supplied minimum, bounded step/chirp input, no irregular capture or overruns, fresh command takes over after STOP; original layout/settings restored |
| F8 | `python host/test/time_smoke.py --cli ./drivelink-cli --endpoint serial:///dev/serial0 --duration 180 --jitter-us 100 --report time-pi-idle.json`, then repeat with `--traffic --report time-pi-traffic.json`. Shared workflow passed on TCP/PTY and bare-board CH343 | Pi GPIO UART at 921600, serial console disabled; 3.3 V TX/RX crossed to ESP GPIO44/43 with common GND; disconnect the CH343 TX drive before connecting Pi TX | Idle/traffic clock stability from production I/O timestamps; exact statistic is p95 held-out low-RTT midpoint residual, not absolute offset accuracy | At least 95% successful pings, mature drift fit, residual <=100 us, prediction change <=2 ms in both runs; restore original settings. Pi run is user-operated in HW |

## Board state

Normal H1c firmware (core v9, time v1, diag v3; ELF build id `af77763ae42b77ea`) runs confirmed in `ota_0`; `ota_1` holds the H2b-2 `f9-candidate` (`2a350ff69da7c0fd`), not selected. Device id `1cdbd446ff38`, empty saved name; layout format 3. The user approved leaving verified H1c installed. Saved layout/settings match the LP checkpoint exactly: factory control/sensor loop instances (type 0x05, ids 0/1), no modules; rates 1000/100/10 Hz, cores 1/0, watchdog 200 ms; idle groups and unconfigured capture. PSRAM capture capacity 4 MiB; XiP and bench crash/stall off; panic-reset Task WDT on. No pending/dirty flags, disarmed. The HIL app is not flashed. Ports/traps: [docs/bench.md](../docs/bench.md).

## Models and effort

- **Plan and build** every step in one session on **Opus 5.5 at high effort**. The session that wrote the plan writes the code; there is no implementation subagent and no model switch after the plan is approved.
- **Review.** The session reviews its own diff. An independent review agent on **Opus** at the step's review effort is required where the "Review" column of [host-steps.md](host-steps.md) says so (concurrency, wire or persistence, arm/disarm, command paths, lease and access), and also when another step's diff touches those paths or is large. The PR says which was done and why.
- **Hardware checks** on the bare bench board are run by the session after the review. Checks needing motors, a real IMU, the Pi or the robot are deferred (above). A check that needs only the PC or a Pi and can't be run by the session is run by the user from a script, and the step waits in `waiting on hardware`.
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's review effort.** If a step turns out harder (or easier) than its row says, change it in the same PR and log the reason.
- **T0 exception (user-approved 2026-11-14).** This Codex session plans and builds T0; self-review at high effort is sufficient for its tooling-only diff. The user approved the T0 plan before edits.

- **H1a exception (user-approved 2026-11-15).** Codex plans/builds H1a, with an independent Codex review at high effort. The user also approved the H1a-1/H1a-2 split; both parts precede H1b/H1c.

- **H1b exception (user-approved 2026-11-15).** Codex plans/builds H1b with an
  independent Codex review at high effort. The user kept H1b in one PR after a
  proposed split. The approved scope and runtime choices are in
  [h1b-implementation.md](h1b-implementation.md).

- **H1c exception (user-approved 2026-11-15).** Codex plans/builds H1c with an independent Codex review at high effort. The user approved the plan and leaving verified normal core v9 installed; see [H1c plan/results](h1c-implementation.md).

- **H2a-2 exception (user-approved 2026-11-15).** Codex plans/builds H2a-2 with independent Codex review at high effort. The user approved the plan and keepalive-only/already-armed observation choices; [plan/results](h2a-2-implementation.md). Review and vdev checks completed 2026-11-16.

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR. Firmware-era rows are in the archive.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-17 | H2d | H2d split into H2d-1 (plots) and H2d-2 (manual command), each independently reviewed; hold rule for H2d-2: only numeric inputs with a finite descriptor range including 0 are commanded, each loop's inputs split at connect into a value group (hold 0) and a keepalive-only group, every value sent is a shown slider value or 0, queued values discarded before this GUI writes a live param to an instance it commands; the Layout live group is released while Layout is not shown; Monitor uses free slots only and refuses an add without one | User decisions 2026-11-17 | yes (plan, host-steps, architecture, host rules) |
| 2026-11-18 | H2d-1 | Internal: a group's plot rate is capped at 1 kHz (`kMaxGroupRateHz`; the firmware link task drains each group once per 1 ms, found on the bench at 2 kHz); a chosen divider rises with a live loop-rate change; at most 16 signals and 8 plots, windows 2–60 s; lost frames = counter gaps; live values for plotted signals only; Resume clears the rings, cursors only while paused; loop rate from the live `rate_hz` param; Monitor groups persist across pages and over a lost link (same device); a failed pass retries 3 times; Snapshot CSV to `Documents/DriveLink/snapshots` with a toast, no dialog; plots and rates not persisted; `PlotItem` paints a cosmetic 1 px image on the software renderer (16 × 10 k samples ≈ 20 ms) and line strips on RHI | Bench result; smallest change that matches the G5 frames; review findings | yes (plan, GUI README, architecture link budget) |
| 2026-11-13 | F6, F10 | Test harnesses retry group-only Busy/StagingBusy rejections (in addition to Withdrawn), bounded to ten attempts; production SDK/CLI stays unchanged. F10 applies the same bounded retry to the native "autonomous loops" group test (`test_core_groups.cpp`) | CI's existing sub-enable scenario raced a loop's atomic slot claim (host-linux in F6; the native test on PR #60's CI, where the owning loop's `endTick` held the slot during `enable`). The rejection occurs before mutation; unit tests and independent review confirm timeouts/actions/expected refusals are not broadened | n/a |

## Hardware results

One row per run in [hardware-log.md](hardware-log.md). Firmware-era results are in [archive/firmware-status.md](archive/firmware-status.md#hardware-test-results).
