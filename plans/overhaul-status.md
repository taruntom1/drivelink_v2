# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** F5b: delete `motor_manager` and the legacy motor streams, and add the loop-stall test, after the F5a-2 PR is merged.
- **No motors or IMU on the bench.** Checks that need them are deferred: each step writes its script now and lists it in "Deferred hardware checks". No step waits in `waiting on hardware`.
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

## Handoff for F5b

F5a-2 adds `actuator.pwm_dir` (0x21) and `feedback.quadrature` (0x22): pure
logic over `mod_motor/hardware.hpp`, a device backend in `mod_motor/src/idf/`,
a fake in the native tests. Only the firmware registers them; the vdev does
not. Core v5, diag v3 and NVS formats are unchanged; the contracts are in
`firmware/components/mod_motor/README.md`.

- **Deleting `motor_manager` and `motor_controller`.** The new actuators use
  `motor_hal` with `Options{exclusive_operator, start_forced}` and
  `write/forceLow/releaseForce`; the encoder uses `Encoder::Options` and
  `readCount`. Once the legacy manager is gone, the pool's shared-operator mode
  and `setSpeed`, and the encoder's on-demand rate sampler and its two Kconfig
  options, have no user left: delete or keep them deliberately.
  `velocity_estimator.hpp` in `motor_controller` still forwards to `mod_motor`.
- **Loop-stall test.** `motor_smoke.py` has the real path (`--modules real`);
  add the stall scenario to both paths. On the bare board, a stalled control
  loop must leave `forced` set on `actuator.pwm_dir` through the monitor alone.
  The bench stall needs a `CONFIG_CORE_BENCH_CRASH=y` build (see
  `smoke_safety_firmware.py --bench`); flash the normal firmware afterwards.
- **A type's diag code 1 is its `InitFailed`**: the core reports it in place of
  `core.InstanceStartFailed`. F6's `imu.bno055` should follow the same pattern.
- **Main stack.** Instance `start()` runs on `main`; F5a-2 raised it to 5632
  bytes (min 1824 free with real modules and their start-failure paths).
- **PC crash during the bench run.** On 2026-11-13 the host PC bugchecked
  (0xD1, a kernel driver fault) while `motor_smoke.py` drove COM3 and a
  pyserial capture held COM4. The rerun with the same setup passed. If it
  recurs, suspect the WCH CH343 (2.1.2025.7) or libwdi USB-JTAG driver, and
  avoid holding COM4 open across many COM3 reboots.
- The board is back on normal firmware with its loops-only layout, rates
  1000/100/10 Hz and watchdog 200 ms. No motors or IMU were run.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further. Then it adds rows here, for example F3a-1 and F3a-2. `waiting on hardware` is unused while the deferred-check rule holds: "deferred" in the hardware column means a script in "Deferred hardware checks", not a wait.

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
| F3d | Configurable loops: loops as instances of the built-in `loop` type, the `realtime` flag in place of the loop classes, the factory layout, the real-time guard in the native tests, core wire v2, host library, CLI and vdev | bare board: a third loop added, saved and reloaded; stack marks; safe mode starts no loop | high | merged | [#41](https://github.com/taruntom1/drivelink_v2/pull/41) |
| F4a | Subscription and command groups (preallocated, epochs), bound to any loop id; core wire v3, host/CLI/vdev | no | high | merged |[#44](https://github.com/taruntom1/drivelink_v2/pull/44) |
| F4b | Arming, `DISARM` fast path, safety monitor, watchdog, `DIAG_RAISE`, `blocks_arm` / `disarms`, `kind: latched`; CLI arm and stream commands; the read-only `limits.*` system params | yes: monitor and watchdog on the board with mock modules | high | merged | [#47](https://github.com/taruntom1/drivelink_v2/pull/47) |
| F4c | Sources: `kind: source` items (the param, its generated value and age signals, the `Source` item flag), `limits.link_values`, the per-loop-pair triple buffers, `Sampled<T>`, `fusion.mock` reading across loops | bare board: the control loop's longest tick with and without a 16-value link | high | merged | [#48](https://github.com/taruntom1/drivelink_v2/pull/48) |
| F5a-1 | Shared motor interfaces, true cascade PID controller, fresh-command bumpless mode switch and realistic mocks; [approved implementation plan](f5a-1-implementation.md) | bare board: controller with mocks; deferred: motors | high | merged | [#50](https://github.com/taruntom1/drivelink_v2/pull/50) |
| F5a-2 | Real PWM+DIR actuator and quadrature feedback modules, hardware emergency force/release and driver failure paths; depends on F5a-1 | bare board: instances on real pins, pool limits, mocks; deferred: motors | high | in review | [#52](https://github.com/taruntom1/drivelink_v2/pull/52) |
| F5b | Delete `motor_manager` and the legacy motor streams; loop-stall test | bare board: loop-stall stop with mocks; deferred: motors | high | planned | |
| F6 | `imu.bno055` module behind a bus interface, with a fake BNO055 bus in the native tests; calibration actions; delete `imu_bmo_055` | bare board: missing-sensor path, calibration actions on `imu.mock`; deferred: IMU | high | planned | |
| F7a | Capture family, PSRAM (caps alloc only) | bare board: capture of mock signals | high | planned | |
| F7b | Test signal generator; evaluate `SPIRAM_XIP_FROM_PSRAM` | bare board: step and chirp on the mock motor at 1 kHz; deferred: on a motor | high | planned | |
| F8 | Time ping with TX stamping; delete `time_sync` | bare board: stable estimate over the CH343; deferred: Pi GPIO-UART jitter | high | planned | |
| F9 | OTA: partition table, OTA family, rollback | bare board: OTA and rollback | high | planned | |
| F10 | Cleanup: leftover Kconfig and legacy registry entries, rewrite `firmware/CLAUDE.md` for the new architecture, remove the temporary overhaul section from the root `CLAUDE.md`, tag `main`'s last legacy commit `legacy-final`, merge `firmware_overhaul` into `main` marked as not yet run on motors or an IMU | bare board: full smoke test | high | planned | |
| HW | Hardware campaign: run every open row of "Deferred hardware checks"; fixes go to `main` as PRs | yes: motors, IMU, Pi, robot | high | planned | |
| H1 | Complete `libdrivelink`: time model, device model, services, plant identification math; host-windows CI. May start in a second session after F4b is merged | no (vdev) | high | planned | |
| H2 | GUI in local mode (Qt/QML): connect, device, layout, tune and monitor screens | vdev; deferred: bench tuning | high | planned | |
| H3 | Agent: arbitration, lease, clock model distribution; GUI remote mode | yes: Pi + PC | high | planned | |
| H4 | `drivelink_hardware`, `drivelink_bringup`, diagnostics node, ROS CI | deferred: robot | high | planned | |

## Notes for later steps

Each note is deleted by the step that uses it.

- **GUI design follow-ups.** [todo-device-name.md](todo-device-name.md) records the proposed device identity and Rename behaviour. It was proposed before F3b landed and must be reconciled with the current core before a wire change is scheduled. [todo-layout-ops.md](todo-layout-ops.md) is closed: the loop at `ADD_INSTANCE` is built, and the GUI follows the firmware's `Referenced` refusal. The GUI design is being brought in line with F3d's loops and F4c's planned sources in phase G7b of [gui-design.md](gui-design.md). That phase found two gaps, both settled on 2026-11-13 and scheduled below: the device's limits (F4b) and a source's value and age as signals (F4c). No firmware changes are authorised by the GUI design sessions.

- **F5**
  - Removing the legacy motor streams (F5b) frees two command group channels.
- **F6**
  - Removing the IMU manager frees `0x42` and the `0xBF` channel.
  - The driver's register traffic goes through a small bus interface. A native test plays the BNO055 from its datasheet: chip id, mode switches, calibration status, a NACK and a timeout. It covers init, the calibration actions and the error paths. On the bare board only the missing-sensor path is checked.
- **F10**
  - Before the merge, tag `main`'s last legacy commit `legacy-final`. Say in `README`/`firmware/CLAUDE.md` that the new firmware has not run on motors or an IMU until HW closes the deferred checks.
- **F7b**
  - Decide whether the test signal generator stays one per loop, now that the number of loops varies.
  - Controller mode changes require a fresh input receipt. Testgen must explicitly participate in the owning-loop receipt/activation contract; writing only the input value would leave a switched controller waiting.
- **F8**
  - The time ping (`0x02` / `0x42`) is not in the registry yet.
- **H1**
  - The F3c host slice has no module-specific code and no cached device or time model.

## Deferred hardware checks

There are no motors or IMU on the bench. A check that needs them doesn't block its step:

- **The step writes the script now** (under `firmware/test/uart_comm/` or `host/test/`) and dry-runs it against vdev or the bare board with mock modules, so the script is known to work. It runs with real modules through a flag, not a second script.
- **It adds a row here:** the step, the script and its command, the hardware it needs, what it checks and what result means pass.
- **When hardware arrives,** the step in progress pauses at a clean point and the rows for that hardware run before any further step. Otherwise they run in the HW step after F10.
- **A motor script starts with the first-run procedure:** wheels off the ground, a current-limited supply, a low duty cap and a hand on the power switch.
- **A row is deleted** once it has passed, with a line in "Hardware test results".

| Step | Script and command | Needs | Checks | Pass |
|---|---|---|---|---|
| F5a-1 / F5a-2 | `python host/test/motor_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --confirm-first-run --pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G] [--duty-cap 0.2] [gains/targets/tolerances]`; the same setup passed on the bare board with `--no-motor --pool-check` | Motor driver, motor and encoder | First-run procedure; encoder polarity in open loop before any closed loop; velocity and position tracking; watchdog force; longest tick. F5b adds loop-stall | Polarity positive; tracking within the given tolerances; after the watchdog trip `forced` is set, applied duty 0 and the encoder stops (at most 2 counts in 0.5 s); zero overruns |

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
- **Saved state on the board now:** the factory control and sensor loop instances (type 0x05, ids 0 and 1), no modules, rates 1000 / 100 / 10 Hz (control, sensor, status), watchdog 200 ms. Normal F5a-2/core v5 firmware is flashed; bench crash/stall are disabled and panic-reset Task WDT is enabled.
- **Bench pins for real modules:** actuator PWM 4 / DIR 5, encoder 6 / 7 (nothing attached; pull-ups keep the count at 0). `motor_smoke.py`'s pool check uses 1, 2, 10–18, 21, 39–42 and 47. Avoid strapping pins 0, 3, 45, 46, the legacy IMU's I2C 8/9, and 38/48 (RGB LED on some boards).
- **Stack rule:** every task keeps more than 1 KB free. Check the marks after adding a task or a large buffer. `app_main`'s locals are large, so look at `main` after adding anything it starts.
- **Latest stack marks** (F5a-2 real modules on pins, both pool-limit layouts and regressions; minimum bytes free across runs; stacks 4096 unless given): main 1824 (5632), uart_rx 2756, uart_tx 3056, motor_mgr_upd 1872, motor_mgr_cfg 3164 (5120), imu_cfg 2092, imu_stream 1780, time_sync_task 1912, diag 1748, esp_timer 3248, core_cfg 4068 (6144), core_loop_0 3072, core_loop_1 3152.
- **Never run on hardware so far:** a real BNO055, motors, a board host other than Windows. F4b armed only mock actuators on the bare board. None of these are expected before HW; see "Deferred hardware checks".

## Other traps

- **CI builds each IDF app from inside its directory.** With `-C` and `-B` together, the build dir resolves against the current directory but `SDKCONFIG` against the project dir, so the variant's paths split.
- **Native timer/task failures** are now injected through core_os/native_task.hpp, including rollback after a later loop rate restart fails.
- **`reboot_required` system params are refused** (`Unsupported`): the system instance is not a layout entry and has no pending copy. None exist today.

## Models and effort

- **Plan and build** every step in one session on **Opus 5.5 at high effort**. The session that wrote the plan writes the code; there is no implementation subagent and no model switch after the plan is approved.
- **Review.** The session reviews its own diff. It also spawns an independent review agent, always on **Opus** at the step's review effort, when the step needs one (wire, safety, persistence or concurrency changes it can't fully cover itself, or a large diff). The PR says which was done and why.
- **Hardware checks** that run on the bare bench board (HIL pytest, mock modules, smoke tests, OTA) are flashed and run by the session after the review, with no help from you. Checks that need motors, a real IMU or the robot are deferred (see "Deferred hardware checks"); the step merges without them. A check that needs only a Pi or a PC is run by you from a script, and the step waits in `waiting on hardware`.
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's review effort.** If a step turns out harder (or easier) than its row says, change its review effort in the same PR and log the reason.
- **Exceptions so far.** F3b and F3c used Codex, approved per step. F3d, F4a, F4b and F4c are explicitly approved for Codex planning/build and an independent Codex review at high effort.
- **F5a-2** used the default policy: planned and built on Opus 5.5 at high
  effort, with an independent Opus review agent at high effort.

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-13 | F5a-2 | `actuator.pwm_dir` forces its one PWM generator low (DIR left alone) and takes an MCPWM operator of its own (6 at most); the pool keeps a shared mode only for the legacy motors | Both generators of an operator share a read-modify-write force register (user decision) | yes |
| 2026-11-13 | F5a-2 | Tick-time driver errors raise latched, disarming `actuator_pwm_dir.DriverFault` / `feedback_quadrature.ReadFailed`; start failures use each type's code-1 `InitFailed` (stage 1 pin unset, 2 driver error) | User decision; code 1 is what the core reports in place of `core.InstanceStartFailed` | yes |
| 2026-11-13 | F5a-2 | `invert` on both types (locked while armed), `pullup` on the encoder, `duty_limit` on the actuator; a reversal passes through one zero tick and a zero write leaves DIR; MCPWM resolution default 10 MHz; main task stack 5632 | User decision (params); internal choices, the stack after an 848-byte mark on the board | yes |
| 2026-11-13 | tooling | Add token-efficiency rules to overhaul-step; use environment-neutral tools and the approved model policy | User request after F5a-1; preserve approval, safety, full validation and independent review gates | yes: skill |
| 2026-11-13 | F5–F10 | Checks that need motors, an IMU or the robot are deferred, not waited for. Each step merges on native, vdev and bare-board checks, and writes and dry-runs a script for each deferred check. A new HW step after F10 runs them; if hardware arrives earlier, the step in progress pauses and they run then | No motors or IMU are available; the user chose not to block the overhaul on them | yes |
| 2026-11-13 | F5b, F6 | `motor_manager` and `imu_bmo_055` are still deleted in F5b and F6, before any hardware check | They ran only in mock mode on this branch, so keeping them proves nothing; `main` and git history keep them (user decision) | n/a |
| 2026-11-13 | F10 | `main`'s last legacy commit is tagged `legacy-final`, then `firmware_overhaul` merges, marked as not yet run on motors or an IMU | User decision; the tag is the fallback firmware | yes |
| 2026-11-13 | F5, F6 | To make up for no hardware: F5a makes the mock motor more realistic (quantization, deadband, saturation, one-tick delay, optional noise); F6 tests the BNO055 driver against a fake bus. Review effort stays high; F5 has no bench pin self-check | User decision | yes |
| 2026-11-13 | HW | The first motor run uses a procedure, not new firmware: wheels off the ground, a current-limited supply, a low duty cap, a hand on the power switch. The GPIO e-stop and driver-enable pin stay later options | User decision | yes |
| 2026-11-13 | F4c | Codex plans/builds with an independent Codex review at high effort; the F4c implementation plan including core v5 and packed source references is approved | Explicit user approval in this session | yes |
| 2026-11-13 | F4b | Codex plans/builds with an independent Codex review at high effort; the F4b plan including core v4 is approved | Explicit user approval in this session | yes |
| 2026-11-13 | F4b | GROUP LIST capacities move to read-only limits params; fast DISARM reuses the ordinary system header | One wire path for capacities; the F4b plan was approved | yes |
| 2026-11-12 | F2 | Every wire change bumps the version byte of the protocol it changes | User decision | yes |
| 2026-11-12 | F3a | A step too large to review in one PR (about 4–5k lines) is split into rows; the session proposes the split in its plan | User decision | n/a |
| 2026-11-13 | F4a | Codex plans/builds with an independent Codex review at high effort; core v3 group plan approved | Explicit user approval for F4a | yes |
| 2026-11-13 | F4a | Command inputs have a single group owner; cross-loop rebinding hands off through the old loop; resolved/staged bindings add RAM beyond packed values | Internal implementation choices, documented with the group contract | yes |
| 2026-11-13 | F4a | Device command inputs stay gated disarmed; host NaN-to-hold mapping follows arming/hold behavior | User-approved F4a/F4b boundary; non-finite wire values remain rejected | yes |
| 2026-11-13 | F3d | Codex plans and builds this step, with an independent Codex review at high effort | Explicit user exception, limited to F3d | n/a |
| 2026-11-13 | F3d | CORE_MAX_LOOPS=1 gets only the factory control loop | User decision during the pre-plan interview | yes |
| 2026-11-13 | F3d | Built-in loop id 0x05; loop capacity separate from module capacity; layout format 2, instance format 1 | Keeps fixed diag source ids clear and preserves the existing module capacity | yes |
| 2026-11-13 | F3d | Loops are instances of a built-in `loop` type, not a separate table with its own ops | Reuses describe, params, the pending layout and NVS A/B (user decision) | yes |
| 2026-11-13 | F3d | A per-loop `realtime` flag with a per-type requirement (`required`, `forbidden`, `any`) replaces the `Control` and `Sensor` loop classes | The classes were only a label once loops are configurable (user decision) | yes |
| 2026-11-13 | F3d | The stall rule stays: a stall of any loop disarms, real-time or not | No safety rule is loosened (user decision) | yes |
| 2026-11-13 | F3d | At most 4 loops by default (Kconfig, up to 8); `rate_hz` 1–2000 for every loop; `control` + `sensor` are the factory layout | User decision | yes |
| 2026-11-13 | F3d | Loop priority is capped at 20; every real-time loop task joins the Task WDT; safe mode starts no loop task; the status stream carries a loop id with each overrun count | The monitor (esp_timer task, 22) must preempt every loop; a bad loop config can be what crashes; loop ids can have gaps. Proposed with the design and not objected to | yes |
| 2026-11-13 | F4b | The device reports its limits as read-only system params (`limits.*`), not in `GET_INFO`. F4b adds the loop, instance and group limits; later steps add their own | No new op and no wire format change; a host can say "4 of 4 loops" before it sends an op. Kept out of F4a, which was already in progress (user decision) | yes |
| 2026-11-13 | tooling | `tools/validate.py` is the one list of checks; CI calls it with `--only`, and a step's new test suite is added to it. Checks run in sequence, the WSL build directory is outside `/tmp`, and a compiler crash is retried once | In F4a three IDF builds at once made a timing test fail, WSL lost a `/tmp` build directory and the no-log build needed a manual retry; the commands were repeated in CI, two guides and the skill (user decision) | n/a |
| 2026-11-13 | tooling | The main firmware builds only `main`, `esp_vfs_console` and what they require (`set(COMPONENTS ...)` in `firmware/CMakeLists.txt`), as the HIL app does. `esp_vfs_console` is named because nothing requires it and the console would otherwise fall back to the ROM output | 701 build steps in place of 1133, and `esp_lcd`, whose compile crashed the Windows compiler, is no longer built. The binary differs only in `esp_err_to_name`, which loses the names of components that are not built | n/a |

## Hardware test results

One row per run. The stack marks and what was never run are in "Hardware notes".

| Date | Step | What was run | Result |
|---|---|---|---|
| 2026-11-12 | F1b | HIL pytest (22 cases) at 921600 baud; legacy smoke, three runs | Pass |
| 2026-11-12 | F3a-1 | Main firmware boot, console read for 65 s (COM4 only) | Pass |
| 2026-11-12 | F3a-2 | Blank-NVS boot; `CONFIG_CORE_BOOT_SELFTEST` save and reload over four resets (COM4 only) | Pass |
| 2026-11-12 | F3b | HIL pytest (22 cases); core smoke with three mocks (layout, params, SAVE, reboot, reload, actions); legacy smoke; a `CORE_BENCH_CRASH` build: three panics, safe mode, EXIT_SAFE_MODE | Pass |
| 2026-11-13 | F3c | CLI serial smoke (`host/test/hardware_smoke.py`); legacy smoke; console stack marks | Pass |
| 2026-11-13 | F3d | CLI serial smoke: third loop and mock, SAVE/reboot/reload, live rate change, actions, status ids, restoration; all task stack marks above 1 KB. Bench build: three panics, safe mode with zero loops, EXIT_SAFE_MODE and cleanup; normal firmware restored; normal core and legacy smoke passed | Pass |
| 2026-11-13 | F4b | Normal firmware: legacy/core/CLI smoke; mock ARM/commands/fast DISARM, stale mailboxes, armed refusals, blockers/fault ACK, watchdog re-arm, read-only limits; every measured stack above 1 KB (minimum 1200). Bench: 150 ms stalls of both loops and recovery; Task WDT panic names core_loop_0, then reboot. Forced reconfigure, normal firmware restored; bench commands refused; final legacy/core/safety smoke and layout/settings restoration passed | Pass |
| 2026-11-13 | F4c | Source value/age smoke; matched 35 s control-loop runs with 0/16 linked values (106/94 us max, zero overruns); all stacks >1 KB (minimum 1200). Legacy/core/CLI/safety smoke passed; core v5/source descriptors/limit verified; original layout, rates, watchdog and idle groups restored | Pass |
| 2026-11-13 | F5a-1 | Normal firmware: 1 kHz cascade/controller-mock control, mode freshness, persistence, watchdog/re-arm; legacy/core/safety/sources/CLI smoke. Matched 0/16-value links: 204/200 us max, zero overruns. All stacks >1 KB (minimum 1200); original layout/settings/groups restored | Pass |
| 2026-11-13 | F5a-2 | `motor_smoke.py --modules real --no-motor --pool-check` (pins 4/5, 6/7): start on real pins, forced while disarmed, release on ARM, duty cap and reversal, still count, watchdog force, re-arm, disarm force; 5th encoder and 7th actuator fail start with `InitFailed` stage 2 / ESP_ERR_NOT_FOUND and ARM is refused (StartFailed). 1 kHz longest tick 184–226 us, zero overruns. Main stack first 848 free, raised to 5632 (min 1824). Mock cascade, safety, core, legacy and CLI smoke. Host PC bugchecked (0xD1) once mid-run; rerun passed. Layout/settings restored | Pass |
