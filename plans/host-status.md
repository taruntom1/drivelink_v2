# Host status

The working state of the host overhaul: the steps in [host-steps.md](host-steps.md), building [host-architecture.md](host-architecture.md) on the `host_overhaul` branch. **Every PR into `host_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

The firmware overhaul (S0, S1, F1–F10, LP) is finished; its step table, decision log and hardware results are archived in [archive/firmware-status.md](archive/firmware-status.md). Its design record is [firmware-architecture.md](firmware-architecture.md).

## Current

- **Host re-planned (2026-11-14).** [host-architecture.md](host-architecture.md) is the design record and [host-steps.md](host-steps.md) holds a brief per step and the items left to step plans. Order: library, GUI in local mode, agent and remote mode, ROS. This file replaced `overhaul-status.md` the same day. [PR #67](https://github.com/taruntom1/drivelink_v2/pull/67).
- **Next step:** H2g (installer and local GUI close-out) after H2f merges. H2f (the custom panel mechanism and the `imu.bno055` calibration panel) is built, self-reviewed and checked on vdev; its real-BNO055 check is a new deferred row. HW runs as hardware arrives.
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

## Handoff for H2g / HW

- **Read first:** [host/gui/README.md](../host/gui/README.md) "Custom panels (H2f)" and [host/gui/modules/imu.bno055/README.md](../host/gui/modules/imu.bno055/README.md) (panel rules, calibration file), the custom panel and Tune rules in [host/CLAUDE.md](../host/CLAUDE.md), and [archive/h2e-implementation.md](archive/h2e-implementation.md) for the Tune records.
- **What exists (H2f):** `host/gui/modules/` (the `drivelink_gui_modules` library: panel QML in its resources, typed C++ `imu.bno055/calibration.*` and the `ImuCalibration` QML singleton, registered by `registerModulePanels()`; `imu.mock/Panel.qml` wraps `imu.bno055`); `Connection::actionResults`/`actionRunning`/`readTextFile`/`writeTextFile` and a `number` on live signal rows; Layout keeps the side column on the panel tab and passes `dialogParent`; `DLCalibrationLevel`, `DLOrientationPreview`; `gui_panel_tests` (ctest `gui-panels`, including the C++ logic tests); screenshot scenarios `layout-calibration`, `layout-calibration-applied`; `drivelink-gui --panel`.
- **For H2g:** `modules/` is in the shell library's resources, so the installer needs nothing extra for it; the installed-GUI check should open the Calibration tab on an `imu.mock` instance. H2f adds no dependency to the third-party notices.
- **Traps:**
  - **`runAction` emits `changed` before it returns.** A panel that advances on `changed` re-enters itself; `CalibrationPanel.send()` guards it (without the guard, apply was re-sent recursively until the JS stack overflowed).
  - **The panel unloads on link loss** (Layout empties); its per-instance memory lives in the `ImuCalibration` singleton (one per engine), keyed by the last non-empty device id.
  - **QML into C++.** `QVariantMap` parameters reject JS `null`/`undefined` (warnings while the panel loads); take `QVariant` where QML can pass them. `QVERIFY2` evaluates its message eagerly: never read `expected::error()` there unguarded.
  - **Parallel builds.** Two ninja runs in one build directory corrupt the static libraries (`ar`/`ranlib` errors); serialize builds when a subagent also builds.
  - **A flaky native test.** `test_configurable_loops.cpp:246` (`REQUIRE(c.revert().ok())`) failed once in H2f's first `full` run and passed three reruns; firmware untouched. Rerun `--only native --force` before suspecting a change.
  - **Irregular vdev ticks, square-wave steps, stale snapshots** (H2e) still hold.
- **Open edges:**
  - **Mode edits elsewhere** (Layout during identification) and **response signal** selection on step runs (H2e).
  - **Remote calibration** needs the lease (H3b).
  - **Panel-only states** (imported review, warnings, apply failed, stale) are covered by tests and one-off renders, not by screenshot scenarios.

## Steps## Steps

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
| H2d-1 | Monitor plots: signal browser with link budget and refusals, GUI-owned subscription groups on free slots, `PlotItem`, stacked plots, pause/cursors/readout, Snapshot CSV, Layout "Plot in Monitor"; Layout live group follows the page. Depends on H2c-2 | vdev: Monitor tests and screenshots; bare board: `benchMonitor` at 41/81/82 % of the link, 1000 Hz measured, 0 lost | high, independent | merged | [#80](https://github.com/taruntom1/drivelink_v2/pull/80) |
| H2d-2 | Monitor manual command: CommandControl per input, the hold rule (value and keepalive-only groups), slider, dead-man, Latch, zero around live writes. Depends on H2d-1 | vdev: command tests, screenshots, `benchCommand` dry run; deferred: motor | high, independent | merged | [#81](https://github.com/taruntom1/drivelink_v2/pull/81) |
| H2e-1 | Tune runs: core v10 (`ItemFlag::Tune`, u16 item flags), step metrics in libdrivelink, Tune page (controller and gains, test signal and capture, Run/Stop, run return to 0, runs list, overlay, pinning, metrics, 1080p plot column). Depends on H2d-2 | vdev: Tune tests, screenshots; bare board: v10 smokes, `benchBoard`, `benchTune` with a mock; deferred: motor | high, independent | merged | [#82](https://github.com/taruntom1/drivelink_v2/pull/82) |
| H2e-2 | Tune session: gain history and restore, A/B compare, Auto re-run (a gain change during a run cancels it), session files, CSV export, autosave and recovery. Depends on H2e-1 | vdev: Tune tests, screenshots, `benchTune` Auto re-run dry run; deferred: motor | high, independent | merged | [#83](https://github.com/taruntom1/drivelink_v2/pull/83) |
| H2e-3 | Tune plant identification: FOPDT fit and SIMC suggestions in libdrivelink, the tab and three cards, the mode switched for the step and written back, CheckItem warning. Depends on H2e-2 | vdev: identification tests, screenshots, `benchIdentify` dry run; deferred: motor | high, independent | merged | [#84](https://github.com/taruntom1/drivelink_v2/pull/84) |
| H2f | IMU calibration custom panel: `gui/modules/<type>/` mechanism, the `imu.bno055` panel (also `imu.mock`), versioned calibration file, generic backend additions, two style controls. Depends on H2c-2 | vdev: panel tests, screenshots, `benchCalibration` dry run; deferred: IMU | high, self | in review | [#85](https://github.com/taruntom1/drivelink_v2/pull/85) |
| H2g | Inno Setup installer, local GUI close-out. Depends on H2e-3, H2f | bare board: installed GUI | high, self | planned | |
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
| H2d-2 | `DRIVELINK_GUI_BENCH_ENDPOINT=serial://COM3 DRIVELINK_GUI_BENCH_INPUT=32:0:514 DRIVELINK_GUI_BENCH_VALUE=0.1 DRIVELINK_GUI_BENCH_RESPONSE=32:0:257 DRIVELINK_GUI_BENCH_MIN_RESPONSE=R gui_monitor_tests benchCommand` with the F5 motor layout (controller `32:0` in `open_loop`, wired to the real actuator and encoder), normal firmware. Dry run passed on vdev with a mock actuator (`DRIVELINK_GUI_BENCH_INPUT=112:0:512`, response `112:0:256`, min 0.5) | Motor driver, motor and encoder | First-run procedure; the GUI's manual command path on a real actuator: duty_sp through the value group, read back over a Monitor subscription; dead-man ramp to 0 on release; Latch held past the watchdog; Hold all; DISARM. Then by hand in the GUI: drag and release, Latch, Esc while dragging | Read-back equals the sent value; velocity magnitude ≥ R while commanded; input 0 after release and after Hold all; still armed after 0.5 s latched; disarmed at the end. By hand: release ramps the motor to rest, Esc stops it at once |
| H2e-1 / H2e-2 | `DRIVELINK_GUI_BENCH_ENDPOINT=serial://COM3 DRIVELINK_GUI_BENCH_TUNE=32:0 DRIVELINK_GUI_BENCH_TARGET=32:0:512 DRIVELINK_GUI_BENCH_RESPONSE=32:0:257 DRIVELINK_GUI_BENCH_STEP=S DRIVELINK_GUI_BENCH_MIN_RESPONSE=R DRIVELINK_GUI_BENCH_GAIN=32:0:32 gui_tune_tests benchTune` with the F5 motor layout (controller `32:0` in `velocity` mode with working gains, wired to the real actuator and encoder), normal v10 firmware. Dry runs passed on vdev (controller mode, with `_GAIN` for H2e-2) and on the bare board with `DRIVELINK_GUI_BENCH_STAGE_MOCK=1` (H2e-1) | Motor driver, motor and encoder | First-run procedure; a Tune step run on `velocity_sp` through the GUI's run path (capture config, arm, START, TestgenDone, read), metrics, the return to 0, Stop mid-run; H2e-2: vel.kp +10 % re-runs by itself, back during a 3 s run cancels it and re-runs, Stop; disarm. Then by hand in the GUI: a gain drag between runs, F5, Stop, Esc mid-run, a slider drag during a run with Auto re-run on, A/B and Restore | Completed run with post-trigger samples, regular timing, response magnitude ≥ R, rise time present; `velocity_sp` reads 0 after each run and after the cancel; Stop gives a stopped run; the auto re-run completes with the new gain; the cancelled run is not kept; vel.kp ends at its value; disarmed at the end. By hand: Esc stops the motor at once and the run shows the disarm marker; a drag during a run stops the motor before the re-run |
| H2e-3 | `DRIVELINK_GUI_BENCH_ENDPOINT=serial://COM3 DRIVELINK_GUI_BENCH_IDENTIFY=32:0 DRIVELINK_GUI_BENCH_TO=0.2 DRIVELINK_GUI_BENCH_MIN_GAIN=K gui_tune_tests benchIdentify` with the F5 motor layout (controller `32:0` in `velocity` mode with working gains, `duty_sp` commanded, wired to the real actuator and encoder), normal v10 firmware; optional `_FROM`, `_DURATION`, `_APPLY=0`. Dry run passed on the sample vdev | Motor driver, motor and encoder | First-run procedure; plant identification through the GUI's path: mode set to `open_loop` for the step and written back, the step input back at 0, a FOPDT model of the velocity, Apply (then the original gains back), a Stop mid-step; disarm. Then by hand in the GUI: the tab, the confirmation, Esc during the step, the speed choice | The mode reads `velocity` after each step and after the Stop; `duty_sp` reads 0; K ≥ the given minimum and positive (polarity), the fit not poor, T and L plausible for the motor; regular timing; disarmed at the end. By hand: Esc stops the motor at once and the mode is written back |
| H2f | `DRIVELINK_GUI_BENCH_ENDPOINT=serial://COM3 DRIVELINK_GUI_BENCH_IMU=48:0 DRIVELINK_GUI_BENCH_CAL_WAIT=300 gui_panel_tests benchCalibration` with an `imu.bno055` instance (sensor loop, SDA 8 / SCL 9, address 0x28, NDOF), normal firmware, no unsaved changes. Dry run passed on a vdev with a calibrated `imu.mock` (`_IMU=114:0`, `_CAL_WAIT=30`) | BNO055, suitable I2C wiring/pull-ups | The operator calibrates while the script prints the levels every 5 s; then through the GUI panel: Read (all four levels at 3), Export, Import of that file (no warnings), Apply (write, then apply), a new valid sample; the staged `calibration.*` params equal the file; REVERT at the end. Then by hand in the GUI: the Calibration tab during the calibration motions (levels, the orientation preview against the sensor's real axes and the heading convention), a file from another device (warning), Apply with Cancel, Esc during Apply | 3/3/3/3 reached; Read, Export and Apply ok and "Applied" with fresh samples; staged params equal the file; levels stay at 3 after Apply; no unsaved changes at the end. By hand: the preview axes turn with the sensor, yaw increases clockwise, the warning shows, Cancel sends nothing |

## Board state

Normal H2e-1 firmware (core v10, time v1, diag v3; ELF build id `59429185fa2d504a`, built from the H2e-1 tree) runs in `ota_0`, flashed over UART with `tools/bench_fw.py normal`; `ota_1` still holds the H2b-2 `f9-candidate` (`2a350ff69da7c0fd`), not selected. Device id `1cdbd446ff38`, empty saved name; layout format 3. The user approved leaving v10 installed (2026-11-18). An unrecorded saved motor layout (cascade_pid `pid`, actuator.pwm_dir `mtr` on PWM 4 / DIR 5, feedback.quadrature `enc` on 6/7, control loop) was found and removed at the user's request (2026-11-18); saved layout/settings are back to the LP checkpoint: factory control/sensor loop instances (type 0x05, ids 0/1), no modules; rates 1000/100/10 Hz, cores 1/0, watchdog 200 ms; idle groups and unconfigured capture. PSRAM capture capacity 4 MiB; XiP and bench crash/stall off (`bench-stall` refused, result 1 detail 9); panic-reset Task WDT on. No pending/dirty flags, disarmed. The HIL app is not flashed. Ports/traps: [docs/bench.md](../docs/bench.md).

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

- **H2f exception (user-approved 2026-11-18).** Opus 5.5 (high) plans and builds the backend additions, the panel logic (typed C++ `calibration.*` and its QML singleton, `CalibrationPanel.qml`; ported from JavaScript at the user's request after the first review), the tests and fixes; a Sonnet 5.5 subagent (high) builds the drawing-only cards, the Apply dialog, `DLCalibrationLevel`, `DLOrientationPreview`, their gallery entries and control tests, and the two screenshot scenarios from the panel's written contract, reviewed by the session. Self-review (the brief's column).

- **H2e-3 exception (user-approved 2026-11-18).** Opus 5.5 (high) plans and builds the backend, tests and fixes; a Sonnet 5.5 subagent (high) builds the QML cards, the CheckItem Warning state and the screenshot option from a written contract, reviewed by the session. Independent review on Opus (high) as required.

- **H2a-2 exception (user-approved 2026-11-15).** Codex plans/builds H2a-2 with independent Codex review at high effort. The user approved the plan and keepalive-only/already-armed observation choices; [plan/results](h2a-2-implementation.md). Review and vdev checks completed 2026-11-16.

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR. Firmware-era rows are in the archive.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-18 | H2f | User: the panel logic in typed C++, not JavaScript. Internal (approved with the plan): module code only in `gui/modules/<type>/` (C++ registered by a generic `registerModulePanels()`), `imu.mock` reuses the `imu.bno055` panel; generic backend additions (`actionResults`, `actionRunning`, bounded text files, a `number` on live rows); file `.imucal.json` v1 of decoded fields with provenance, refused for another type, warned for another device, instance or low levels; Read needs 3/3/3/3; Apply = write then apply, "Applied" after two new valid sample counts (after 3 s "no new sample yet"); not gated by arm state beyond the generic paused-action rule; side column kept beside the panel, no "Module context" card; panel memory per device id and instance while the GUI runs | G7 and F6; vdev behaviour; no wire or safety change | yes (gui-design, host-steps, GUI and module READMEs, host rules, imu.md) |
| 2026-11-13 | F6, F10 | Test harnesses retry group-only Busy/StagingBusy rejections (in addition to Withdrawn), bounded to ten attempts; production SDK/CLI stays unchanged. F10 applies the same bounded retry to the native "autonomous loops" group test (`test_core_groups.cpp`) | CI's existing sub-enable scenario raced a loop's atomic slot claim (host-linux in F6; the native test on PR #60's CI, where the owning loop's `endTick` held the slot during `enable`). The rejection occurs before mutation; unit tests and independent review confirm timeouts/actions/expected refusals are not broadened | n/a |

## Hardware results

One row per run in [hardware-log.md](hardware-log.md). Firmware-era results are in [archive/firmware-status.md](archive/firmware-status.md#hardware-test-results).
