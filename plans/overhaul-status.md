# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** F3d: configurable loops and the real-time flag. F4a follows it.
- **Older notes.** This file was cut down on 2026-11-13. The full handoff notes, decision log and hardware results up to F3c are in git: `git show b734af5:plans/overhaul-status.md`.

## Keeping this file short

Every session reads all of this file, so it holds only what has no other home.

- **A fact goes where its subject lives.** If it is already there, don't repeat it here.

  | Subject | Home |
  |---|---|
  | Wire format, result and detail codes, diag codes | `docs/protocol/` (`core.md`, `diag.md`, `framing.md`, `definitions.md`, generated `registry.md`, `diag-codes.md`) |
  | Firmware rules, build and test commands | `firmware/CLAUDE.md` and the area files it lists (`main/`, `test/`, `components/`, `uart_comm/`, `legacy-managers.md`), `protocol/CLAUDE.md` |
  | Core API and behaviour | `firmware/components/core/README.md`, `firmware/components/core_os/README.md` |
  | Host rules, build and usage | `host/CLAUDE.md`, `host/README.md`, `host/vdev/README.md` |
  | Design and its reasons | `plans/firmware-architecture.md`, `plans/host-architecture.md` |

- **The handoff is replaced, not appended.** Write what the next step must know. Move anything a later step needs to "Notes for later steps", under that step.
- **A note is deleted by the step that uses it.**
- **A decision log row stays while a later step needs it.** Once the plan, a doc or a README states it, the next session that edits this file removes the row.
- **Hardware results:** one short row per run. Replace the stack marks with the newest.

## Handoff for F3d

The design and the user's decisions are in `plans/firmware-architecture.md`, "Configurable loops, the real-time flag and sources". F3d builds the loops and the flag. F4c builds the sources.

- **Where the two loops are hard-coded today:**
  - `core/loop.hpp`: `kControlLoop`, `kSensorLoop`, `kLoopCount`, `loopClassOf`
  - `core/core.hpp`: `m_control`, `m_sensor`, the two `PeriodicTask`s, the `loop()` and `task()` ternaries
  - `core.cpp`: the system instance's rate params, `applySystemParam`, REVERT's four lists, `loopStackHighWater`, the two start stages
  - `param_service`: `ParamDomain::Control` / `Sensor` and the loop array
  - `command_service.cpp`: ADD_INSTANCE's default loop and the GET_INFO loop list
  - `idf/core_link.cpp`: `overruns[2]` in the status stream
  - `CoreConfig::control` / `sensor` and their Kconfig options
  - `firmware/main/main.cpp`: the task names in the stack log (`core_ctrl`, `core_sens`)
  - `system.yaml` ids `0x0001` and `0x0002`
  - `diag_codes.yaml`: `core.InitFailed` stages 2 and 3, `core.TimerRestartFailed` arg0
  - `host/libdrivelink/src/client.cpp`: the JSON for loops
  - docs that describe the two loops: `firmware/CLAUDE.md`, `firmware/main/CLAUDE.md` (the `core.InitFailed` stages), and "Loops and concurrency" and the Kconfig table in `firmware/components/core/README.md`
- **The loop class has three uses:** the type check (`layout_validation.cpp`), the refusal of paused actions while armed (`Core::runAction`), and ADD_INSTANCE's default loop.
- **A paused loop still counts its heartbeat** (`Loop::runTick`). Only the refusal in `Core::runAction` keeps a real-time loop from being paused while armed.
- **`core.InitFailed` stage numbers are wire contract.** Keep 2 as "a loop task" with the loop id in arg1, and retire 3.
- **The loop type id** must stay clear of the fixed diag sources (`0x01`–`0x04`, `0x10`–`0x12`). `reportStartFailed` in `core.cpp` treats type ids from `0x20` up as diag sources of their own.
- **NVS namespaces.** Each distinct `t??i??` uses one of NVS's 254 namespace slots for good, even after an erase. Loop instances use them too.
- **`CONFIG_CORE_BOOT_SELFTEST`** (main) flips `system.control.rate_hz`, which F3d retires. SAVE is on the wire now, so remove the self-test.
- **It is a wire change (core v2).** Update `docs/protocol/core.md`, the host library, the CLI, the vdev, `smoke_core_firmware.py` and `host/test/hardware_smoke.py` in the same PR.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further. Then it adds rows here, for example F3a-1 and F3a-2.

| Step | Content | Hardware check | Review effort | State | PR |
|---|---|---|---|---|---|
| S0 | Workflow: this file, the temporary `CLAUDE.md` section, CI on `firmware_overhaul`, the `/overhaul-step` skill | no | high | merged | [#28](https://github.com/taruntom1/drivelink_v2/pull/28) |
| S1 | Repo restructure: firmware into `firmware/` | no | high | merged | [#29](https://github.com/taruntom1/drivelink_v2/pull/29) |
| F1a | Framing, CRC, scanner and `wire_codec` extracted into `protocol/`; `uart_comm` fixes for big frames | no | high | merged | [#30](https://github.com/taruntom1/drivelink_v2/pull/30) |
| F1b | New framing: u16 LEN + HCRC8, `MAX_PAYLOAD` 1024, 921600 baud, UART ISR in IRAM | yes: HIL pytest | xhigh | merged | [#31](https://github.com/taruntom1/drivelink_v2/pull/31) |
| F2 | `tools/gen/`, `protocol/defs/`, generated registry, descriptor format and hash | no | high | merged | [#33](https://github.com/taruntom1/drivelink_v2/pull/33) |
| F3a-1 | `core_os`, `firmware/test/native`, module registry and bind API, loops, param service | bare board: boot check | high | merged | [#34](https://github.com/taruntom1/drivelink_v2/pull/34) |
| F3a-2 | Layout and pending copy, validation, NVS A/B, safe mode, boot in dependency order | bare board: boot, save/reload | high | merged | [#35](https://github.com/taruntom1/drivelink_v2/pull/35) |
| F3b | System, layout, describe, param and action families; status stream; `GET_INFO`; diag redesign (wire v3); `mod_mock` | yes: mock instances on the board | high | merged | [#36](https://github.com/taruntom1/drivelink_v2/pull/36) |
| F3c | `drivelink-vdev`, first `libdrivelink` slice, `drivelink-cli`, `host/superbuild`, host-linux CI | bare board: CLI serial smoke | high | merged | [#37](https://github.com/taruntom1/drivelink_v2/pull/37) |
| F3d | Configurable loops: loops as instances of the built-in `loop` type, the `realtime` flag in place of the loop classes, the factory layout, the real-time guard in the native tests, core wire v2, host library, CLI and vdev | bare board: a third loop added, saved and reloaded; stack marks; safe mode starts no loop | high | in progress | |
| F4a | Subscription and command groups (preallocated, epochs), bound to any loop id | no | high | planned | |
| F4b | Arming, `DISARM` fast path, safety monitor, watchdog, `DIAG_RAISE`, `blocks_arm` / `disarms`, `kind: latched`; CLI arm and stream commands | yes: monitor and watchdog on the board with mock modules | high | planned | |
| F4c | Sources: source params, the per-loop-pair triple buffers, `Sampled<T>`, `fusion.mock` reading across loops | bare board: the control loop's longest tick with and without a 16-value link | high | planned | |
| F5a | Motor module types (actuator, feedback, controller) with `emergencyStop`, `onArmTransition`, bumpless mode switch | yes: motors | high | planned | |
| F5b | Delete `motor_manager` and the legacy motor streams; loop-stall test on hardware | yes: motors | high | planned | |
| F6 | `imu.bno055` module and calibration actions; delete `imu_bmo_055` | yes: IMU | high | planned | |
| F7a | Capture family, PSRAM (caps alloc only) | yes: capture | high | planned | |
| F7b | Test signal generator; evaluate `SPIRAM_XIP_FROM_PSRAM` | yes: motors | high | planned | |
| F8 | Time ping with TX stamping; delete `time_sync` | yes: time jitter | high | planned | |
| F9 | OTA: partition table, OTA family, rollback | yes: OTA and rollback | high | planned | |
| F10 | Cleanup: leftover Kconfig and legacy registry entries, rewrite `firmware/CLAUDE.md` for the new architecture, remove the temporary overhaul section from the root `CLAUDE.md`, merge `firmware_overhaul` into `main` | yes: full smoke test | high | planned | |
| H1 | Complete `libdrivelink`: time model, device model, services, plant identification math; host-windows CI. May start in a second session after F4b is merged | no (vdev) | high | planned | |
| H2 | GUI in local mode (Qt/QML): connect, device, layout, tune and monitor screens | yes: bench tuning | high | planned | |
| H3 | Agent: arbitration, lease, clock model distribution; GUI remote mode | yes: Pi + PC | high | planned | |
| H4 | `drivelink_hardware`, `drivelink_bringup`, diagnostics node, ROS CI | yes: robot | high | planned | |

## Notes for later steps

Each note is deleted by the step that uses it.

- **F4a**
  - The command groups have 29 channels (`kCmdGroupChannelCount`) while the legacy streams sit on `0xBD`–`0xBF`. The Kconfig group count must `static_assert` against it.
  - The host's `Session` facade takes group services and stream decoding without a change to the transport.
  - Write the "latch a list of bound signals at the end of the tick" code so that F4c's links can reuse it.
- **F4b**
  - The armed checks already exist and are tested through `ParamService::setArmed()`: layout ops, SAVE, the reboot paths, paused actions and `locked_while_armed` params. Nothing sets it on the device yet, and ARM, DISARM and ACK_FAULT answer `Unsupported`.
  - `Instance` has no `onArmTransition` or `emergencyStop` yet.
  - Diag state has 254 fixed sparse identities. The `blocks_arm` and `disarms` flags are in the metadata with false defaults; the latch and arming behaviour is missing.
  - The monitor walks the loop table. See the paused-loop heartbeat note in the F3d handoff.
- **F4c**
  - `ItemFlag` has one free bit (`0x80`). `GET_ITEMS` also carries a `ref` string, which could mark a source instead.
- **F5**
  - `feedback.mock` casts its ref to the concrete mock actuator and reads the previous tick's values. Real actuator and feedback interfaces replace that.
  - Removing the legacy motor streams (F5b) frees two command group channels.
- **F6**
  - Removing the IMU manager frees `0x42` and the `0xBF` channel.
- **F7b**
  - Decide whether the test signal generator stays one per loop, now that the number of loops varies.
- **F8**
  - The time ping (`0x02` / `0x42`) is not in the registry yet.
- **H1**
  - The F3c host slice has no module-specific code and no cached device or time model.

## Hardware notes

- **The board:** a bare ESP32-S3 (QFN56 rev 0.2, 8 MB PSRAM). No motor and no BNO055 are attached, so the legacy IMU manager runs in mock mode.
- **Ports on this PC:**
  - COM3 is the CH343 adapter on UART0 (GPIO43/44). It carries the protocol and also flashes the board through DTR/RTS.
  - COM4 is the native USB port, the console.
- **First open resets the board.** The first time the adapter's port is opened after a flash, the board resets and the first command is lost. `smoke_main_firmware.py` retries its first query for this reason.
- **Windows serial in `libdrivelink`.** Asio's `flow_control(none)` reasserts DTR/RTS and resets the board, so keep the bootstrap lines that `open()` leaves disabled. A native Win32 `AccessDenied` or `FileNotFound` at reopen is retried before any request is sent.
- **Smoke scripts:**
  - `firmware/test/uart_comm/smoke_main_firmware.py`: the legacy managers. It creates no motor.
  - `firmware/test/uart_comm/smoke_core_firmware.py`: the core with mock instances, which it removes again. `--safe-mode` needs a `CONFIG_CORE_BENCH_CRASH=y` build; flash the normal firmware again afterwards.
  - `host/test/hardware_smoke.py`: the same through the CLI. It refuses to run with pending edits, and restores the layout and rates it found.
- **Saved state on the board now:** a zero-instance layout and rates 1000 / 100 / 10 Hz (control, sensor, status).
- **Stack rule:** every task keeps more than 1 KB free. Check the marks after adding a task or a large buffer. `app_main`'s locals are large, so look at `main` after adding anything it starts.
- **Latest stack marks** (F3c, bytes free; stacks are 4096 unless given): main 1304 (4608), uart_rx 3164, uart_tx 3096, motor_mgr_upd 1880, motor_mgr_cfg 3156 (5120), imu_cfg 2084, imu_stream 1996, time_sync_task 1792, diag 2048, esp_timer 3352, core_ctrl 3256, core_sens 3260, core_cfg 4352 (6144).
- **Never run on hardware so far:** a real BNO055, motors, an armed core, a host other than Windows.

## Other traps

- **CI builds each IDF app from inside its directory.** With `-C` and `-B` together, the build dir resolves against the current directory but `SDKCONFIG` against the project dir, so the variant's paths split.
- **`core.TimerRestartFailed`** (the rollback of a failed rate change) can't be triggered in the native tests.
- **`reboot_required` system params are refused** (`Unsupported`): the system instance is not a layout entry and has no pending copy. None exist today.

## Models and effort

- **Plan and build** every step in one session on **Opus 5.5 at high effort**. The session that wrote the plan writes the code; there is no implementation subagent and no model switch after the plan is approved.
- **Review.** The session reviews its own diff. It also spawns an independent review agent, always on **Opus** at the step's review effort, when the step needs one (wire, safety, persistence or concurrency changes it can't fully cover itself, or a large diff). The PR says which was done and why.
- **Hardware checks** that run on the bare bench board (HIL pytest, mock modules, smoke tests, OTA) are flashed and run by the session after the review, with no help from you. Checks that need motors, a real IMU, the robot, a Pi or a PC are run by you from a script, and the step waits in `waiting on hardware`.
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's review effort.** If a step turns out harder (or easier) than its row says, change its review effort in the same PR and log the reason.
- **Exceptions so far.** F3b and F3c used Codex, approved per step. F3d is also explicitly approved for Codex planning/build and an independent Codex review at high effort.

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-12 | F2 | Every wire change bumps the version byte of the protocol it changes | User decision | yes |
| 2026-11-12 | F3a | A step too large to review in one PR (about 4–5k lines) is split into rows; the session proposes the split in its plan | User decision | n/a |
| 2026-11-13 | F3d | Codex plans and builds this step, with an independent Codex review at high effort | Explicit user exception, limited to F3d | n/a |
| 2026-11-13 | F3d | CORE_MAX_LOOPS=1 gets only the factory control loop | User decision during the pre-plan interview | yes |
| 2026-11-13 | F3d | Built-in loop id 0x05; loop capacity separate from module capacity; layout format 2, instance format 1 | Keeps fixed diag source ids clear and preserves the existing module capacity | yes |
| 2026-11-13 | F3d | Loops are instances of a built-in `loop` type, not a separate table with its own ops | Reuses describe, params, the pending layout and NVS A/B (user decision) | yes |
| 2026-11-13 | F3d | A per-loop `realtime` flag with a per-type requirement (`required`, `forbidden`, `any`) replaces the `Control` and `Sensor` loop classes | The classes were only a label once loops are configurable (user decision) | yes |
| 2026-11-13 | F3d | The stall rule stays: a stall of any loop disarms, real-time or not | No safety rule is loosened (user decision) | yes |
| 2026-11-13 | F3d | At most 4 loops by default (Kconfig, up to 8); `rate_hz` 1–2000 for every loop; `control` + `sensor` are the factory layout | User decision | yes |
| 2026-11-13 | F3d | Loop priority is capped at 20; every real-time loop task joins the Task WDT; safe mode starts no loop task; the status stream carries a loop id with each overrun count | The monitor (esp_timer task, 22) must preempt every loop; a bad loop config can be what crashes; loop ids can have gaps. Proposed with the design and not objected to | yes |
| 2026-11-13 | F4c | A source whose producer is missing does not stop its consumer; the consumer runs and sees `valid == false` | A fusion module can go on with what it has (user decision) | yes |
| 2026-11-13 | F3d, F4c | Two new steps: F3d (loops and the flag) before F4a, and F4c (sources) after F4b | F4a, F4b and F5 are then built on the loop table; F4c reuses F4a's latch code | yes |

## Hardware test results

One row per run. The stack marks and what was never run are in "Hardware notes".

| Date | Step | What was run | Result |
|---|---|---|---|
| 2026-11-12 | F1b | HIL pytest (22 cases) at 921600 baud; legacy smoke, three runs | Pass |
| 2026-11-12 | F3a-1 | Main firmware boot, console read for 65 s (COM4 only) | Pass |
| 2026-11-12 | F3a-2 | Blank-NVS boot; `CONFIG_CORE_BOOT_SELFTEST` save and reload over four resets (COM4 only) | Pass |
| 2026-11-12 | F3b | HIL pytest (22 cases); core smoke with three mocks (layout, params, SAVE, reboot, reload, actions); legacy smoke; a `CORE_BENCH_CRASH` build: three panics, safe mode, EXIT_SAFE_MODE | Pass |
| 2026-11-13 | F3c | CLI serial smoke (`host/test/hardware_smoke.py`); legacy smoke; console stack marks | Pass |
