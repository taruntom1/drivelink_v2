# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** F4b: arming, safety monitor and watchdog, after the F4a PR is merged.
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

## Handoff for F4b

F4a adds preallocated GroupService resources and core wire v3 across firmware,
host, CLI and vdev. NVS formats are unchanged. See docs/protocol/core.md and
core/README.md for the wire, memory and ownership contract.

- Device loops always call beginTick with the disarmed default. Connect the
  real arm state/instant in F4b, implement transitions and clear old mailboxes.
  Command values are only applied when received strictly after ARM; valid
  prefix-only keepalives update receipt time without reviving stale values.
- lastCommandUs returns a bool plus a timestamp: false means its nonblocking
  snapshot was unavailable, not that no frame arrived. F4b must not conclude
  timeout from an unavailable read. Reconfiguration clears the receipt time.
- Config/clear refuse while armed for commands, and command inputs have one
  group owner. Subscription mutations remain allowed. Empty command groups
  can carry keepalives. ARM must still require a configured command group.
- Core::loopCapacity/loopAt enumerate slots; skip nulls. Group bindings use
  actual loop ids, with loop statistics sampled on their owning loop.
- Paused loops count heartbeat; paused actions on any real-time loop remain
  refused while armed. All running loops must participate in stall monitoring.
- Host command() is explicit and nonblocking with a bounded queue. The CLI
  cmd-send drains with an ordered read. Automatic keepalives, ARM/stream/CSV
  workflows and NaN-to-hold conversion follow the arming/hold behavior.
- No firmware was flashed for F4a (its planned check is native/vdev). The bench
  board still runs F3d/core v2; rebuild and flash v3 before using the new host.

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
| F3d | Configurable loops: loops as instances of the built-in `loop` type, the `realtime` flag in place of the loop classes, the factory layout, the real-time guard in the native tests, core wire v2, host library, CLI and vdev | bare board: a third loop added, saved and reloaded; stack marks; safe mode starts no loop | high | merged | [#41](https://github.com/taruntom1/drivelink_v2/pull/41) |
| F4a | Subscription and command groups (preallocated, epochs), bound to any loop id; core wire v3, host/CLI/vdev | no | high | in review |[#44](https://github.com/taruntom1/drivelink_v2/pull/44) |
| F4b | Arming, `DISARM` fast path, safety monitor, watchdog, `DIAG_RAISE`, `blocks_arm` / `disarms`, `kind: latched`; CLI arm and stream commands; the read-only `limits.*` system params | yes: monitor and watchdog on the board with mock modules | high | planned | |
| F4c | Sources: `kind: source` items (the param, its generated value and age signals, the `Source` item flag), `limits.link_values`, the per-loop-pair triple buffers, `Sampled<T>`, `fusion.mock` reading across loops | bare board: the control loop's longest tick with and without a 16-value link | high | planned | |
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

- **GUI design follow-ups.** [todo-device-name.md](todo-device-name.md) records the proposed device identity and Rename behaviour. It was proposed before F3b landed and must be reconciled with the current core before a wire change is scheduled. [todo-layout-ops.md](todo-layout-ops.md) is closed: the loop at `ADD_INSTANCE` is built, and the GUI follows the firmware's `Referenced` refusal. The GUI design is being brought in line with F3d's loops and F4c's planned sources in phase G7b of [gui-design.md](gui-design.md). That phase found two gaps, both settled on 2026-11-13 and scheduled below: the device's limits (F4b) and a source's value and age as signals (F4c). No firmware changes are authorised by the GUI design sessions.

- **F4b**
  - The armed checks already exist and are tested through `ParamService::setArmed()`: layout ops, SAVE, the reboot paths, paused actions and `locked_while_armed` params. Nothing sets it on the device yet, and ARM, DISARM and ACK_FAULT answer `Unsupported`.
  - `Instance` has no `onArmTransition` or `emergencyStop` yet.
  - Diag state has 254 fixed sparse identities. The `blocks_arm` and `disarms` flags are in the metadata with false defaults; the latch and arming behaviour is missing.
  - The monitor walks the loop table. See the paused-loop heartbeat note in the current handoff.
  - **Limits as read-only system params** (plan: "Limits" under the notes on the ops). Add to `system.yaml`: `limits.loops`, `limits.instances`, and the group limits F4a built (`limits.sub_groups`, `limits.cmd_groups`, `limits.group_entries`).
    - Each is `u16`, `read_only`, and takes its value from `CoreConfig` at boot. The system instance must write it into the record's shadow, because a param's boot value otherwise comes from its YAML default.
    - No wire format changes; the descriptor hash does. Test: `GET` returns the build's values, `SET` answers `ReadOnly`, and SAVE writes none of them.
    - First check how F4a exposed its group limits. If it reports them another way (for example in the group `LIST` reply), keep one way only and say so in the decision log.
- **F4c**
  - Reuse core::BoundSignal and latchSignals from F4a for end-of-tick snapshots.
  - **A source is three items** (plan: "Sources", "Declaration" and "What the host sees"). `kind: source` in the YAML expands into the param (flag `Source`, `ItemFlag` `0x80`, the last free bit) and the signals `<name>.value` and `<name>.age_us`. There is no source op.
    - Generator: the expansion, the name rule, unique ids for all three, and tests in `tools/gen/tests`. `docs/protocol/definitions.md` gets the new kind.
    - `age_us` is `0xFFFFFFFF` with no valid sample and saturates at `0xFFFFFFFE`.
    - The flag bit is a wire change: bump the core version, and update `docs/protocol/core.md`, the host library and the CLI in the same PR.
  - Add `limits.link_values` (`CORE_MAX_LINK_VALUES`) beside the limits F4b added.
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
- **Saved state on the board now:** the factory control and sensor loop instances (type 0x05, ids 0 and 1), no modules, rates 1000 / 100 / 10 Hz (control, sensor, status). Normal firmware is flashed; BenchCrash is disabled.
- **Stack rule:** every task keeps more than 1 KB free. Check the marks after adding a task or a large buffer. `app_main`'s locals are large, so look at `main` after adding anything it starts.
- **Latest stack marks** (F3d, temporary third loop and mocks; bytes free; stacks 4096 unless given): main 1304 (4608), uart_rx 3004, uart_tx 3096, motor_mgr_upd 1880, motor_mgr_cfg 3156 (5120), imu_cfg 2084, imu_stream 1788, time_sync_task 2000, diag 1576, esp_timer 3320, core_loop_0 3172, core_loop_1 3260, core_loop_2 3256, core_cfg 4052 (6144).
- **Never run on hardware so far:** a real BNO055, motors, an armed core, a host other than Windows.

## Other traps

- **CI builds each IDF app from inside its directory.** With `-C` and `-B` together, the build dir resolves against the current directory but `SDKCONFIG` against the project dir, so the variant's paths split.
- **Native timer/task failures** are now injected through core_os/native_task.hpp, including rollback after a later loop rate restart fails.
- **`reboot_required` system params are refused** (`Unsupported`): the system instance is not a layout entry and has no pending copy. None exist today.

## Models and effort

- **Plan and build** every step in one session on **Opus 5.5 at high effort**. The session that wrote the plan writes the code; there is no implementation subagent and no model switch after the plan is approved.
- **Review.** The session reviews its own diff. It also spawns an independent review agent, always on **Opus** at the step's review effort, when the step needs one (wire, safety, persistence or concurrency changes it can't fully cover itself, or a large diff). The PR says which was done and why.
- **Hardware checks** that run on the bare bench board (HIL pytest, mock modules, smoke tests, OTA) are flashed and run by the session after the review, with no help from you. Checks that need motors, a real IMU, the robot, a Pi or a PC are run by you from a script, and the step waits in `waiting on hardware`.
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's review effort.** If a step turns out harder (or easier) than its row says, change its review effort in the same PR and log the reason.
- **Exceptions so far.** F3b and F3c used Codex, approved per step. F3d and F4a are explicitly approved for Codex planning/build and an independent Codex review at high effort.

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
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
| 2026-11-13 | F4c | A source whose producer is missing does not stop its consumer; the consumer runs and sees `valid == false` | A fusion module can go on with what it has (user decision) | yes |
| 2026-11-13 | F4b | The device reports its limits as read-only system params (`limits.*`), not in `GET_INFO`. F4b adds the loop, instance and group limits; later steps add their own | No new op and no wire format change; a host can say "4 of 4 loops" before it sends an op. Kept out of F4a, which was already in progress (user decision) | yes |
| 2026-11-13 | F4c | A source's sampled value and its age are ordinary signals generated from a `kind: source` item; the param carries a new `Source` flag (`0x80`); `age_us` of `0xFFFFFFFF` means no valid sample. No source status op | Groups, capture and plots work on them unchanged, and no second path from a loop to the config task is needed (user decision) | yes |
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
| 2026-11-13 | F3d | CLI serial smoke: third loop and mock, SAVE/reboot/reload, live rate change, actions, status ids, restoration; all task stack marks above 1 KB. Bench build: three panics, safe mode with zero loops, EXIT_SAFE_MODE and cleanup; normal firmware restored; normal core and legacy smoke passed | Pass |
