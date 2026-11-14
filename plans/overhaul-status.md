# Overhaul status

The working state of the overhaul: the firmware ([firmware-architecture.md](firmware-architecture.md), done in F1–F10) and the host ([host-architecture.md](host-architecture.md)). **Every PR into `host_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Instruction cleanup (2026-11-14):** scoped instructions shortened; existing command walkthroughs moved to `docs/development.md`. CLAUDE sources and AGENTS links retained; no step/runtime policy changed. [PR #65](https://github.com/taruntom1/drivelink_v2/pull/65); existing H1a/HW handoff remains applicable.

- **Branches.** F1–F10 are merged into `main`, `legacy-final` resolves to `7a4d693`, and `origin/host_overhaul` exists (verified 2026-11-14). The HW and H steps run on `host_overhaul`; `main` stays frozen until `host_overhaul` merges, and HW firmware fixes go to `host_overhaul` too.
- **Next step:** H1a or HW. LP (automatic initial loop placement) is merged in [#63](https://github.com/taruntom1/drivelink_v2/pull/63); H1a needs no hardware, and HW runs as hardware arrives.
- **No motors or IMU on the bench.** Checks that need them are deferred: each step writes its script now and lists it in "Deferred hardware checks". No step waits in `waiting on hardware` for them.
- **Older notes.** This file was cut down on 2026-11-13. The full handoff notes, decision log and hardware results up to F3c are in git: `git show b734af5:plans/overhaul-status.md`. The decision log rows up to F9 are in `git show 272575d:plans/overhaul-status.md`.

## Keeping this file short

Every session reads all of this file, so it holds only what has no other home.

- **A fact goes where its subject lives.** If it is already there, don't repeat it here.

  | Subject | Home |
  |---|---|
  | Wire format, result and detail codes, diag codes | `docs/protocol/` (`core.md`, `diag.md`, `framing.md`, `definitions.md`, generated `registry.md`, `diag-codes.md`) |
  | Firmware rules, build and test commands | `firmware/CLAUDE.md` and the area files it lists (`main/`, `test/`, `components/`, `uart_comm/`), `protocol/CLAUDE.md` |
  | Core API and behaviour | `firmware/components/core/README.md`, `firmware/components/core_os/README.md` |
  | Host rules, build and usage | `host/CLAUDE.md`, `host/README.md`, `host/vdev/README.md` |
  | Design and its reasons | `plans/firmware-architecture.md`, `plans/host-architecture.md` |

- **The handoff is replaced, not appended.** Write what the next step must know. Move anything a later step needs to "Notes for later steps", under that step.
- **A note is deleted by the step that uses it.**
- **A decision log row stays while a later step needs it.** Once the plan, a doc or a README states it, the next session that edits this file removes the row.
- **Hardware results:** one short row per run. Replace the stack marks with the newest.

## Handoff for H1a (and HW)

F10 is docs, tooling and dead config only: no wire format, safety or runtime change. The generator lost its `legacy:` registry field, which changed the descriptor hash (`0xDBE8D3E4`) but no constant. The firmware is core v8, time v1, diag v3. Its docs describe the code as it is: `firmware/CLAUDE.md`, the area files, every component README, `docs/protocol/`, `docs/adding-a-module.md`. `plans/firmware-architecture.md` is the design record; finished step plans are in `plans/archive/`.

- **Before the first H step.** The branch/tag prerequisites were verified in LP (2026-11-14): `legacy-final` on `7a4d693`, firmware in `main`, and `origin/host_overhaul` present.
- **H1a scope** is in [host-architecture.md](host-architecture.md) ("libdrivelink", "Phases"): the typed public API and device model (JSON only in the CLI, CLI output unchanged so every smoke script keeps working), the asynchronous request path, the seqlock group buffers, periodic time ownership and automatic timestamp mapping over F8's `ClockEstimator`, and the host-windows CI job. It is large: propose a split in the plan if it passes about 4–5k lines.
- **H1b** is device identity (core v9; [todo-device-name.md](todo-device-name.md) has the proposal and three open questions for the user) and the plant identification math. It changes the firmware, so it has a bare-board check.
- **GUI records synchronized (2026-11-14, [#64](https://github.com/taruntom1/drivelink_v2/pull/64)).** Merged the accepted G7/G7b records from `main` into `host_overhaul`, retaining LP's discovered placement enum and Any-core Add loop default. G8 discussion is next; applying the placement follow-up to Figma remains open. See [gui-design-status.md](gui-design-status.md).
- **LP handoff.** Loop placement is a discovered enum (`core_0` 0, `core_1` 1, `any` 2); new loops default Any, factory control/sensor remain 1/0. Saved explicit 0/1 values persist. The descriptor hash is now `0xDF9F21A5`, still core v8/time v1/diag v3, with no new wire encoding. DISCARD cancels pending placement edits; REVERT stays live-only. GUI/H2 must consume the enum and use Any core as the Add loop default; Figma follow-up remains. [Plan and results](todo-loop-affinity.md).
- **Bench state.** Normal LP firmware (build id b8ad3d153466c34d, from the d8b2878 working tree) is running in ota_0, with no pending confirmation, the original layout/settings and idle state restored. The HIL test app was built, not flashed (`uart_comm` unchanged). Existing motor/IMU/Pi checks stay deferred.

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
| F5a-1 | Shared motor interfaces, true cascade PID controller, fresh-command bumpless mode switch and realistic mocks; [approved implementation plan](archive/f5a-1-implementation.md) | bare board: controller with mocks; deferred: motors | high | merged | [#50](https://github.com/taruntom1/drivelink_v2/pull/50) |
| F5a-2 | Real PWM+DIR actuator and quadrature feedback modules, hardware emergency force/release and driver failure paths; depends on F5a-1 | bare board: instances on real pins, pool limits, mocks; deferred: motors | high | merged | [#52](https://github.com/taruntom1/drivelink_v2/pull/52) |
| F5b | Delete `motor_manager`, `motor_controller`, the legacy motor streams and the HAL/PID APIs only they used; loop-stall test (`motor_smoke.py --stall`) | bare board: loop-stall stop with mocks and real modules; deferred: motors | high | merged | [#53](https://github.com/taruntom1/drivelink_v2/pull/53) |
| F6 | `imu.bno055`, pure bus/driver and fake-bus tests; persistent calibration through SAVE; delete `imu_bmo_055`; [approved plan](archive/f6-implementation.md) | bare board: missing sensor/ARM gate, mock signals/calibration/SAVE/restore and stacks; deferred: IMU | high | merged | [#55](https://github.com/taruntom1/drivelink_v2/pull/55) |
| F7a | Capture family, PSRAM (caps alloc only), core events, core v6; [approved plan](archive/f7a-implementation.md) | bare board: capture of mock signals | high | merged | [#56](https://github.com/taruntom1/drivelink_v2/pull/56) |
| F7b | Per-loop testgen, fresh-command handoff, capture trigger/events, core v7; XiP evaluation; [approved plan/results](archive/f7b-implementation.md) | bare board: step/chirp at 1 kHz, stacks, XiP comparison and restoration; deferred: motor | high | merged | [#57](https://github.com/taruntom1/drivelink_v2/pull/57) |
| F8 | Time v1, TX stamping, host estimator/API/CLI and removal of `time_sync`; [approved plan/results](archive/f8-implementation.md) | bare board: HIL, stable CH343 idle/traffic estimates and stacks; deferred: Pi GPIO-UART jitter | high | merged | [#58](https://github.com/taruntom1/drivelink_v2/pull/58) |
| F9 | Core v8 OTA, preserved NVS/dual slots, prepared-image reboot commit and host confirmation; [plan/results](archive/f9-implementation.md) | bare board: migration, OTA/rollback, restoration and stacks | high | merged | [#59](https://github.com/taruntom1/drivelink_v2/pull/59) |
| F10 | Cleanup: the generator's legacy registry field, empty Kconfig files, stale legacy text; `firmware/CLAUDE.md` and component docs rewritten, READMEs for the five support components, `docs/adding-a-module.md`; finished plans archived; host plan reconciled with the firmware and H1 split; workflow moved to `host_overhaul`. After merge: tag `legacy-final`, merge into `main`, cut `host_overhaul` | bare board: full smoke test | high | merged | [#60](https://github.com/taruntom1/drivelink_v2/pull/60) |
| HW | Hardware campaign on `host_overhaul`: run every open row of "Deferred hardware checks"; firmware fixes go to `host_overhaul` as PRs | yes: motors, IMU, Pi, robot | high | planned | |
| LP | Automatic initial loop placement: discovered Core 0/Core 1/Any core enum; new loops default Any, factory loops retain 1/0; saved values, host/vdev workflow, bench FPU-affinity evidence and GUI handoff. [Plan/results](todo-loop-affinity.md) | bare board: placement, stacks and Any-core safety passed | high | merged | [#63](https://github.com/taruntom1/drivelink_v2/pull/63) |
| H1a | Typed `libdrivelink`: device model, typed services, async request path, seqlock group buffers; periodic time and automatic timestamp mapping over F8's estimator; CLI on the typed API (JSON formatting moves there, output unchanged); host-windows CI | no (vdev) | high | planned | |
| H1b | Device identity: id and name in `GET_INFO` and a set-name op (core v9, firmware + host + vdev + docs); plant identification math. Depends on H1a | bare board: identity, name persistence, smoke | high | planned | |
| H2 | GUI in local mode (Qt/QML): connect, device, layout, tune and monitor screens, per the Figma design. Depends on H1b | vdev; deferred: bench tuning | high | planned | |
| H3 | Agent: arbitration, lease, clock model distribution, agent MSG_TYPEs; GUI remote mode | yes: Pi + PC | high | planned | |
| H4 | `drivelink_hardware`, `drivelink_bringup`, diagnostics node, ROS CI | deferred: robot | high | planned | |

## Notes for later steps

Each note is deleted by the step that uses it.

- **GUI design follow-ups.** [todo-device-name.md](todo-device-name.md) is scheduled for H1b. The GUI design is being brought in line with the loops and sources in phase G7b of [gui-design.md](gui-design.md); H2 builds from the accepted design. No firmware changes are authorised by the GUI design sessions.
- **Loop placement follow-up.** [todo-loop-affinity.md](todo-loop-affinity.md) was accepted on 2026-11-14 and implementation was authorized in the same Codex chat. New loops default to Any core; factory control/sensor loops stay pinned to cores 1/0. The discovered choices and Add loop default are recorded in the GUI design handoff; Figma edits remain for the next GUI session/H2.
- **H1a**
  - F8 supplies the reusable clock estimator, ping API and CLI measurement. H1a adds periodic ownership and automatic timestamp mapping; H3 distributes the agent clock model. The host still has no module-specific code.
- **H3**
  - Allocate the agent-only MSG_TYPEs in `registry.yaml` from free command, response and event values; they never reach the device.

## Deferred hardware checks

There are no motors or IMU on the bench. A check that needs them doesn't block its step:

- **The step writes the script now** (under `firmware/test/uart_comm/` or `host/test/`) and dry-runs it against vdev or the bare board with mock modules, so the script is known to work. It runs with real modules through a flag, not a second script.
- **It adds a row here:** the step, the script and its command, the hardware it needs, what it checks and what result means pass.
- **When hardware arrives,** the step in progress pauses at a clean point and the rows for that hardware run before any further step. Otherwise they run in the HW step after F10.
- **A motor script starts with the first-run procedure:** wheels off the ground, a current-limited supply, a low duty cap and a hand on the power switch.
- **A row is deleted** once it has passed, with a line in "Hardware test results".

| Step | Script and command | Needs | Checks | Pass |
|---|---|---|---|---|
| F5a-1 / F5a-2 / F5b | `python host/test/motor_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --confirm-first-run --pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G] [--duty-cap 0.2] [gains/targets/tolerances]`, normal firmware; then the same with `--stall` on a `CONFIG_CORE_BENCH_CRASH=y` build (flash the normal firmware afterwards). Both passed on the bare board with `--no-motor` (`--pool-check` on the normal build) | Motor driver, motor and encoder | First-run procedure; encoder polarity in open loop before any closed loop; velocity and position tracking; watchdog force; longest tick; with `--stall`, a 2 s control-loop stall in velocity mode | Polarity positive; tracking within the given tolerances; after the watchdog trip `forced` is set, applied duty 0 and the encoder stops (at most 2 counts in 0.5 s); zero overruns in the normal run (a stall run counts its stall as overruns); stall: Fault/LoopStalled during the stall, the encoder moves at most 25% of its free run at the pre-stall speed, `forced` and zero duty after it |
| F6 | `python host/test/imu_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --sda 8 --scl 9 --address 0x28 --calibration-wait 300` (normal firmware; add `--stack-wait 45` for stack observation). Mock workflow and real `--missing-sensor` both passed on the bare board | BNO055, suitable I2C wiring/pull-ups | Signal groups and stationary plausibility; operator reaches levels 3/3/3/3; exact profile apply and boot restore in CONFIG; NDOF recovery; layout/settings restoration | Valid fresh samples with plausible quaternion/gravity/temperature; 3/3/3/3 before profile capture; exact CONFIG profile readback and all staged params survive SAVE/reboot; modes/cleanup pass; all task stacks >1 KB. Physical orientation/calibration quality must also be checked on the sensor |
| F7b | `python host/test/testgen_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --confirm-first-run --pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G --duty-cap 0.2 --probe-duty 0.1 --min-response R] --loop-rate 1000` (normal firmware). Same workflow passed with mocks on vdev and bare board | Motor driver, motor and encoder | First-run procedure; positive step before chirp, generated input/response at 1 kHz, STOP/hold/fresh handoff and watchdog cancellation | Positive response above the supplied minimum, bounded step/chirp input, no irregular capture or overruns, fresh command takes over after STOP; original layout/settings restored |
| F8 | `python host/test/time_smoke.py --cli ./drivelink-cli --endpoint serial:///dev/serial0 --duration 180 --jitter-us 100 --report time-pi-idle.json`, then repeat with `--traffic --report time-pi-traffic.json`. Shared workflow passed on TCP/PTY and bare-board CH343 | Pi GPIO UART at 921600, serial console disabled; 3.3 V TX/RX crossed to ESP GPIO44/43 with common GND; disconnect the CH343 TX drive before connecting Pi TX | Idle/traffic clock stability from production I/O timestamps; exact statistic is p95 held-out low-RTT midpoint residual, not absolute offset accuracy | At least 95% successful pings, mature drift fit, residual <=100 us, prediction change <=2 ms in both runs; restore original settings. Pi run is user-operated in HW |

## Hardware notes

- **The board:** a bare ESP32-S3 (QFN56 rev 0.2, 8 MB PSRAM). No motor and no BNO055 are attached. Real IMU missing-sensor checks fail cleanly; mock IMU checks use explicit `imu.mock` instances.
- **Ports on this PC:**
  - COM3 is the CH343 adapter on UART0 (GPIO43/44). It carries the protocol and also flashes the board through DTR/RTS.
  - COM4 is the native USB port, the console.
- **First open resets the board.** The first time the adapter's port is opened after a flash, the board resets and the first command is lost. `smoke_protocol_firmware.py` retries its first query for this reason.
- **Windows serial in `libdrivelink`.** Asio's `flow_control(none)` reasserts DTR/RTS and resets the board, so keep the bootstrap lines that `open()` leaves disabled. A native Win32 `AccessDenied` or `FileNotFound` at reopen is retried before any request is sent.
- **Smoke scripts:**
  - `firmware/test/uart_comm/smoke_protocol_firmware.py`: diag v3 and time v1 on the main firmware. It creates no instance.
  - `firmware/test/uart_comm/smoke_core_firmware.py`: the core with mock instances, which it removes again. `--safe-mode` needs a `CONFIG_CORE_BENCH_CRASH=y` build; flash the normal firmware again afterwards.
  - `host/test/hardware_smoke.py`: the same through the CLI. It refuses to run with pending edits, and restores the layout and rates it found.
- **Saved state on the board now:** the factory control and sensor loop instances (type 0x05, ids 0 and 1), no modules, rates 1000 / 100 / 10 Hz (control, sensor, status), cores 1/0, watchdog 200 ms. Normal LP/core v8 + time v1 firmware (ELF build id b8ad3d153466c34d, from the d8b2878 working tree) runs in ota_0 without pending confirmation, with the dual-slot table and PSRAM (capture capacity 4 MiB). XiP/bench crash/stall are off; panic-reset Task WDT is enabled.
- **Bench build:** from the repo root, `python tools/bench_fw.py bench` builds `CONFIG_CORE_BENCH_CRASH=y` in isolated, git-ignored `firmware/build_bench/` and flashes COM3 without touching `firmware/sdkconfig`. Restore with `python tools/bench_fw.py normal`, which reconfigures/builds and checks the generated header has bench and XiP relocation disabled before flashing. `xip` uses isolated `firmware/build_xip/` and the committed evaluation fragment; always restore normal afterwards. IDF export and logs are automatic; see `firmware/main/CLAUDE.md` ("Bench-only options"). `ota-image` builds a distinct normal-config candidate in isolated `firmware/build_ota/` and never flashes; see the main guide. All four subcommands support `--dry-run`; none runs in validation. Verify bench commands are refused after restoration.
- **Serial status backlog.** On Windows over the CH343, each new CLI process reads queued status frames, not the newest one, so a check that polls `status` sees device events late. Judge timing from device data (timestamps, overrun counts), as `motor_smoke.py --stall` does.
- **Bench pins for real modules:** actuator PWM 4 / DIR 5, encoder 6 / 7 (nothing attached; pull-ups keep the count at 0). `motor_smoke.py`'s pool check uses 1, 2, 10–18, 21, 39–42 and 47. Avoid strapping pins 0, 3, 45, 46, the example IMU's I2C 8/9, and 38/48 (RGB LED on some boards).
- **Stack rule:** every task keeps more than 1 KB free. Check the marks after adding a task or a large buffer. `app_main`'s locals are large, so look at `main` after adding anything it starts.
- **LP placement stack marks** (normal, bytes free): main 3688, uart_rx 3164, uart_tx 3176, diag 1744, esp_timer 3352, core_cfg 4272, core_loop_0 3180, core_loop_1 3252, temporary core_loop_2 3168, core_loop_3 3192. F10's deeper OTA/safety hold reached core_cfg 2064 and diag 1628; LP did not repeat OTA. Main stack 5632, core_cfg 6144, others 4096. No OTA/time task. COM4 capture is closed.

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
- **F6**, **F7b**, **F8** and **F9** are explicitly approved for Codex planning/build and independent Codex review at high effort.
- **F5b** and **F7a** used the default policy: planned and built on Opus 5.5
  at high effort, with an independent Opus review agent at high effort.
- **F10** used the default policy, self-review only (docs, tooling and dead
  config; no wire, safety or runtime change).

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-14 | LP | New loops default to Any core; factory loops stay pinned 1/0. Expose enum 0/1/2, map 2 explicitly to no affinity, keep saved 0/1 and existing pending/ARM rules. Descriptor-only update keeps core v8 | User selected the new-loop default and accepted the plan; existing enum/scalar encodings suffice. REVERT remains live-only; DISCARD cancels pending edits | yes |
| 2026-11-14 | F10 | After F10: tag `legacy-final` on `7a4d693`, merge `firmware_overhaul` into `main`, then run HW and H1a–H4 on a new `host_overhaul` branch with the same per-step workflow; `main` stays frozen until it merges, and HW fixes go to `host_overhaul` | User decisions in the F10 planning interview | yes |
| 2026-11-14 | F10 | Host API is typed (device model, typed services); JSON leaves the public `libdrivelink` API and stays only in the CLI, whose output is unchanged | User decision | yes |
| 2026-11-14 | F10 | Device id and name (todo-device-name) go into H1b as core v9; H1 is split into H1a (typed API, device model, time mapping, host-windows CI) and H1b (identity, plant identification) | User decision (identity into H1); the split keeps each PR reviewable | yes |
| 2026-11-14 | F10 | The registry's `legacy:` field and range-tail logic are removed; ranges hold no entries. The descriptor hash changes (0xD05D60D2 to 0xDBE8D3E4), no constant or wire format does | No legacy entry remains; the hash covers the normalized definitions, which lost the field | n/a |
| 2026-11-14 | F10 | Retired diag sources and codes (`motor_manager` 0x10, `time_sync` 0x11, `system` 1–2) stay in the YAML as retired; `smoke_main_firmware.py` is renamed `smoke_protocol_firmware.py`; the five support components get READMEs; finished step plans move to `plans/archive/` | Numbers are never reused; names and docs match the code (internal choices) | n/a |
| 2026-11-13 | F6, F10 | Test harnesses retry group-only Busy/StagingBusy rejections (in addition to Withdrawn), bounded to ten attempts; production SDK/CLI stays unchanged. F10 applies the same bounded retry to the native "autonomous loops" group test (`test_core_groups.cpp`) | CI's existing sub-enable scenario raced a loop's atomic slot claim (host-linux in F6; the native test on PR #60's CI, where the owning loop's `endTick` held the slot during `enable`). The rejection occurs before mutation; unit tests and independent review confirm timeouts/actions/expected refusals are not broadened | n/a |
| 2026-11-13 | tooling | Capture integration PASS lines in both host validation logs; use `tools/bench_fw.py` for isolated bench builds and guarded normal restoration; bound smoke failure frames; apply the skill's token-efficiency rules | F5b repeated manual builds, hidden test stdout, large failure output, host-clock traps and duplicated review searches. Self-review only: tooling/diagnostics, unchanged firmware runtime and test pass criteria; helper help/dry runs and fail-closed tests, no flash | n/a |
| 2026-11-13 | F5–F10 | Checks that need motors, an IMU or the robot are deferred, not waited for. Each step merges on native, vdev and bare-board checks, and writes and dry-runs a script for each deferred check. A new HW step after F10 runs them; if hardware arrives earlier, the step in progress pauses and they run then | No motors or IMU are available; the user chose not to block the overhaul on them | yes |
| 2026-11-13 | HW | The first motor run uses a procedure, not new firmware: wheels off the ground, a current-limited supply, a low duty cap, a hand on the power switch. The GPIO e-stop and driver-enable pin stay later options | User decision | yes |
| 2026-11-12 | F3a | A step too large to review in one PR (about 4–5k lines) is split into rows; the session proposes the split in its plan | User decision | n/a |
| 2026-11-13 | tooling | `tools/validate.py` is the one list of checks; CI calls it with `--only`, and a step's new test suite is added to it. Checks run in sequence, the WSL build directory is outside `/tmp`, and a compiler crash is retried once | In F4a three IDF builds at once made a timing test fail, WSL lost a `/tmp` build directory and the no-log build needed a manual retry; the commands were repeated in CI, two guides and the skill (user decision) | n/a |
| 2026-11-13 | tooling | The main firmware builds only `main`, `esp_vfs_console` and what they require (`set(COMPONENTS ...)` in `firmware/CMakeLists.txt`), as the HIL app does. `esp_vfs_console` is named because nothing requires it and the console would otherwise fall back to the ROM output | 701 build steps in place of 1133, and `esp_lcd`, whose compile crashed the Windows compiler, is no longer built. The binary differs only in `esp_err_to_name`, which loses the names of components that are not built | n/a |

## Hardware test results

One row per run. The stack marks and what was never run are in "Hardware notes".

| Date | Step | What was run | Result |
|---|---|---|---|
| 2026-11-14 | LP | Guarded bench and normal builds; shared loop-placement CLI smoke on COM3/COM4 with real-time/background mocks, save/reboot/readback, stacks and bench affinity snapshots. `smoke_safety_firmware.py --any-core` in normal and `--bench --wdt` modes; final diag/time smoke. [Results](todo-loop-affinity.md#results-2026-11-14) | Pass; Any loops initially unpinned then both pinned to core 1; factory 1/0 retained. Zero observed overruns at 50 Hz, stacks >1 KiB (normal minimum 1744 B). Normal image and original layout/settings/idle state restored, bench op refused |
| 2026-11-12 | F1b | HIL pytest (22 cases) at 921600 baud; legacy smoke, three runs | Pass |
| 2026-11-12 | F3a-1 | Main firmware boot, console read for 65 s (COM4 only) | Pass |
| 2026-11-12 | F3a-2 | Blank-NVS boot; `CONFIG_CORE_BOOT_SELFTEST` save and reload over four resets (COM4 only) | Pass |
| 2026-11-12 | F3b | HIL pytest (22 cases); core smoke with three mocks (layout, params, SAVE, reboot, reload, actions); legacy smoke; a `CORE_BENCH_CRASH` build: three panics, safe mode, EXIT_SAFE_MODE | Pass |
| 2026-11-13 | F3c | CLI serial smoke (`host/test/hardware_smoke.py`); legacy smoke; console stack marks | Pass |
| 2026-11-13 | F3d | CLI serial smoke: third loop and mock, SAVE/reboot/reload, live rate change, actions, status ids, restoration; all task stack marks above 1 KB. Bench build: three panics, safe mode with zero loops, EXIT_SAFE_MODE and cleanup; normal firmware restored; normal core and legacy smoke passed | Pass |
| 2026-11-13 | F4b | Normal firmware: legacy/core/CLI smoke; mock ARM/commands/fast DISARM, stale mailboxes, armed refusals, blockers/fault ACK, watchdog re-arm, read-only limits; every measured stack above 1 KB (minimum 1200). Bench: 150 ms stalls of both loops and recovery; Task WDT panic names core_loop_0, then reboot. Forced reconfigure, normal firmware restored; bench commands refused; final legacy/core/safety smoke and layout/settings restoration passed | Pass |
| 2026-11-13 | F4c | Source value/age smoke; matched 35 s control-loop runs with 0/16 linked values (106/94 us max, zero overruns); all stacks >1 KB (minimum 1200). Legacy/core/CLI/safety smoke passed; core v5/source descriptors/limit verified; original layout, rates, watchdog and idle groups restored | Pass |
| 2026-11-13 | F5a-1 | Normal firmware: 1 kHz cascade/controller-mock control, mode freshness, persistence, watchdog/re-arm; legacy/core/safety/sources/CLI smoke. Matched 0/16-value links: 204/200 us max, zero overruns. All stacks >1 KB (minimum 1200); original layout/settings/groups restored | Pass |
| 2026-11-13 | F5b | Bench build: `motor_smoke.py --stall` with mocks at 1 kHz and with real modules `--no-motor` (pins 4/5, 6/7): Fault/LoopStalled sent at loop-0 overruns 0, before the 2 s stall added 1999; zero duty and `forced` after it, ACK_FAULT and re-arm. `smoke_safety_firmware.py --bench --wdt`. Normal firmware restored (bench op refused, `--stall` refused before any change): legacy smoke without the motor check, core, safety and CLI smoke, mock cascade, real `--no-motor --pool-check` (7th actuator and 5th encoder fail start, ARM refused; longest tick 161 us, zero overruns). Stack marks all >1 KB (minimum 1780). Layout/settings restored | Pass |
| 2026-11-13 | F5a-2 | `motor_smoke.py --modules real --no-motor --pool-check` (pins 4/5, 6/7): start on real pins, forced while disarmed, release on ARM, duty cap and reversal, still count, watchdog force, re-arm, disarm force; 5th encoder and 7th actuator fail start with `InitFailed` stage 2 / ESP_ERR_NOT_FOUND and ARM is refused (StartFailed). 1 kHz longest tick 184–226 us, zero overruns. Main stack first 848 free, raised to 5632 (min 1824). Mock cascade, safety, core, legacy and CLI smoke. Host PC bugchecked (0xD1) once mid-run; rerun passed. Layout/settings restored | Pass |
| 2026-11-13 | F7a | Normal firmware (PSRAM) via `bench_fw.py normal`; `capture_smoke.py --expect-psram` at 1 kHz: capacity 4 MiB, threshold step response (trigger at 250, peak 5.0), manual TRIGGER at divider 10 with CaptureDone event, ABORT/WrongState, longest tick 51–53 us (1 ref) / 68–73 us (16 refs), never irregular; COM4 stack capture during an armed capture (minimum 1792); legacy, core, safety and CLI smoke. Original loops/rates/watchdog restored | Pass |
| 2026-11-13 | F6 | Normal firmware via `bench_fw.py normal`; `imu_smoke.py` mock signals/calibration/rejection/SAVE/reboot/modes and real `--missing-sensor` on 8/9 (failed instance, InitFailed, ARM StartFailed); COM4 stack captures during stable holds (minimum 1580); legacy time_sync/diag, core, safety and CLI smoke. Final original loops 1000/100 Hz, status 10 Hz, watchdog 200 ms, idle groups; bench stall refused | Pass; real sensor/calibration quality deferred |
| 2026-11-14 | F7b | Normal and isolated XiP firmware via guarded helper; capture 1/16 refs and generated step/chirp at 1 kHz on controller/motor mocks, disarmed SAVE measurement; normal restored, 45 s log-chirp/capture stack observation (minimum 1644 B). Final legacy/core/safety/CLI and 1 kHz mock-cascade smoke; original rates/watchdog/status/layout and idle groups/capture/testgen restored, normal bench op refused. [Measured results](archive/f7b-implementation.md#measured-results-2026-11-14) | Pass; real motor deferred |
| 2026-11-14 | F8 | 22 UART HIL cases; guarded normal firmware, protocol/core/safety/CLI smokes before/after; 180 s CH343 idle and 1 kHz subscription/capture runs: held-out residual p95 37.8/207.0 us, prediction-change p95 4.2/26.4 us; stacks all >1 KB, legacy task absent, original layout/settings/volatile state restored and bench op refused. [Results](archive/f8-implementation.md#measured-results-2026-11-14) | Pass; Pi jitter deferred |
| 2026-11-14 | F10 | Normal F10 via `bench_fw.py normal`: protocol, core, sources (0/16 links 215/226 us, zero overruns), safety, CLI, capture `--expect-psram`, motor/IMU/testgen mocks, real `--missing-sensor` and `--no-motor --pool-check`, 90 s CH343 time (residual p95 68 us, prediction change p95 7.7 us). Bench: safe mode, `--stall`, `--bench --wdt`; normal restored, bench op refused. `ota_smoke.py` with the F10 baseline and an `ota-image` candidate; final protocol/core smoke. HIL app not flashed (`uart_comm` unchanged) | Pass; F10 confirmed in ota_0, original layout/settings, all stacks >1 KiB (minimum 1628 B) |
| 2026-11-14 | F9 | Guarded UART migration (24 KiB NVS unchanged); shared OTA smoke: duplicate/conflict/hash/partial reset, prepared ABORT, confirmed boot, unconfirmed rollback and baseline/settings restoration; main time/diag, core, safety and CLI regressions; console stacks | Pass; normal F9 confirmed in ota_0, original layout/settings and idle state; all stacks >1 KiB (minimum 1576 B). [Results](archive/f9-implementation.md#results-2026-11-14) |
