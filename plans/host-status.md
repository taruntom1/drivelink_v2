# Host status

The working state of the host overhaul: the steps in [host-steps.md](host-steps.md), building [host-architecture.md](host-architecture.md) on the `host_overhaul` branch. **Every PR into `host_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

The firmware overhaul (S0, S1, F1–F10, LP) is finished; its step table, decision log and hardware results are archived in [archive/firmware-status.md](archive/firmware-status.md). Its design record is [firmware-architecture.md](firmware-architecture.md).

## Current

- **Host re-planned (2026-11-14).** [host-architecture.md](host-architecture.md) is the design record and [host-steps.md](host-steps.md) holds a brief per step and the items left to step plans. Order: library, GUI in local mode, agent and remote mode, ROS. This file replaced `overhaul-status.md` the same day. [PR #67](https://github.com/taruntom1/drivelink_v2/pull/67).
- **Next step:** H1a after the T0 PR merges. HW runs as hardware arrives.
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

## Handoff for H1a (and HW)

The firmware is core v8, time v1, diag v3 (descriptor hash `0xDF9F21A5` after LP). Its docs describe the code as it is: `firmware/CLAUDE.md`, the area files, every component README, `docs/protocol/`, `docs/adding-a-module.md`.

- **T0** is in review; H1a starts after its PR merges. All seven full-gate checks passed, including the installed C++23 consumer on both host platforms. No hardware check is needed. Updated toolchain commands are in [docs/development.md](../docs/development.md).
- **H1a** is its brief in [host-steps.md](host-steps.md): typed types, errors and services, the device model as a snapshot, the CLI on the typed API with golden output, and the host-windows CI job. The async path, events, group buffers and time mapping are H1b; identity is H1c ([todo-device-name.md](todo-device-name.md)); plant identification is H2e.
- **LP.** Loop placement is a discovered enum (`core_0` 0, `core_1` 1, `any` 2); new loops default to Any, factory control/sensor stay 1/0. DISCARD cancels pending placement edits; REVERT stays live-only. The GUI consumes the enum (H2c).

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further and add rows here (for example H3a-1 and H3a-2). S0, S1, F1–F10 and LP are merged; see the archive.

| Step | Content | Hardware check | Review effort | State | PR |
|---|---|---|---|---|---|
| T0 | Tooling: C++23 for the host and native tests, MSYS2 UCRT64 GCC 16.2 for the native tests, native-tests CI in `ubuntu:26.04` | no | high, self | in review | [#68](https://github.com/taruntom1/drivelink_v2/pull/68) |
| H1a | Typed `libdrivelink`: types, errors, services, device model snapshot; CLI on the typed API (output unchanged); host-windows CI. Depends on T0 | bare board: CLI smokes | high, independent | planned | |
| H1b | Async path, events into the model, seqlock and sample-ring group buffers, command keepalives, periodic time and timestamp mapping. Depends on H1a | bare board: time, 1 kHz groups, smokes | high, independent | planned | |
| H1c | Device identity, core v9 (firmware, host, vdev, docs). Depends on H1a; may run beside H1b | bare board: identity, name persistence, smokes | high, independent | planned | |
| H2a | GUI foundation: app, Theme from Figma tokens (G8 folded in), style, shell, DISARM/Esc, Qt adapters, test and screenshot tooling, vdev scenario hooks, `setup_linux.sh`. Depends on H1b, H1c | vdev | high, independent | planned | |
| H2b | Connect and Device screens (rename, OTA, link lost, reboot notice). Depends on H2a | bare board: GUI scenarios | high, independent | planned | |
| H2c | Layout screen. Depends on H2b | vdev | high, self | planned | |
| H2d | Monitor screen. Depends on H2c | vdev | high, independent | planned | |
| H2e | Tune screen, run metrics, plant identification, session files. Depends on H2d | vdev; deferred: motor | high, independent | planned | |
| H2f | IMU calibration custom panel. Depends on H2c | vdev; deferred: IMU | high, self | planned | |
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

- **H2a.** G8 is folded in: export the Figma tokens and assign the open GUI handoff issues to the screen steps ([gui-design-status.md](gui-design-status.md)). Applying LP's placement enum to the Figma frames is optional.
- **H2c.** Build Add loop and loop detail from the discovered placement enum, with Any core as the Add loop default ([todo-loop-affinity.md](archive/todo-loop-affinity.md)).

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

Normal LP firmware (core v8, time v1; ELF build id b8ad3d153466c34d, from the d8b2878 working tree) runs in ota_0 without pending confirmation, with the dual-slot table and PSRAM (capture capacity 4 MiB). Saved layout: the factory control and sensor loop instances (type 0x05, ids 0 and 1) and no modules; rates 1000 / 100 / 10 Hz (control, sensor, status), cores 1/0, watchdog 200 ms; idle groups. XiP and bench crash/stall are off; the panic-reset Task WDT is on. The HIL test app is not flashed. Ports and traps: [docs/bench.md](../docs/bench.md).

## Models and effort

- **Plan and build** every step in one session on **Opus 5.5 at high effort**. The session that wrote the plan writes the code; there is no implementation subagent and no model switch after the plan is approved.
- **Review.** The session reviews its own diff. An independent review agent on **Opus** at the step's review effort is required where the "Review" column of [host-steps.md](host-steps.md) says so (concurrency, wire or persistence, arm/disarm, command paths, lease and access), and also when another step's diff touches those paths or is large. The PR says which was done and why.
- **Hardware checks** on the bare bench board are run by the session after the review. Checks needing motors, a real IMU, the Pi or the robot are deferred (above). A check that needs only the PC or a Pi and can't be run by the session is run by the user from a script, and the step waits in `waiting on hardware`.
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's review effort.** If a step turns out harder (or easier) than its row says, change it in the same PR and log the reason.
- **T0 exception (user-approved 2026-11-14).** This Codex session plans and builds T0; self-review at high effort is sufficient for its tooling-only diff. The user approved the T0 plan before edits.

## Decision log

Choices that the plans don't cover or that change them, and that a later step must respect. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR. Firmware-era rows are in the archive.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-15 | T0 | The native subscription handoff test waits up to 100 ms for publication after each successful configure and checks all 100 epoch/value samples; production code is unchanged | Ubuntu 26.04 CI exposed a test scheduling assumption: configure acknowledges the beginTick swap before endTick publishes data, so immediate reads can collect zero samples | Test comment |
| 2026-11-14 | planning | `overhaul-status.md` is archived unchanged as `archive/firmware-status.md`; this file replaces it for the host. The bench reference moves to `docs/bench.md`, hardware results to `hardware-log.md`; open deferred checks stay here. Finished or superseded plans are archived | User decision: the old file was 40 KB, mostly finished firmware history read by every session | yes |
| 2026-11-14 | planning | Host re-planned with the user into T0 and H1a–H6 ([host-steps.md](host-steps.md)): library → local GUI → agent and remote → ROS; H1a split in two (H1b async/time) with identity as H1c; GUI as a foundation plus screen slices; G8 folded into H2a; plant identification in H2e. Also decided: C++23 host (GCC 13.1 floor, `protocol/` C++20), typed exceptions with `std::expected` completions, the async dispatcher contract, Qt 6.12.0 with a 6.10 floor, Windows-only GUI product with a Linux CI build, GUI tests with scenario screenshots, JSON session files, Inno Setup; the agent on localhost with opt-in LAN, token and mDNS, actions as lease-only operations, install script then `.deb`; SET_NAME live + `nvs_dirty`; WSL with `setup_linux.sh` and `wsl_serial.py`, no devcontainer; no Python bindings; no firmware change beyond identity; independent review by risk | User decisions in the host planning interview. Planning-session choices: SET_NAME is an exclusive agent operation, `setup_linux.sh` lands in H2a (first step needing Linux packages), H2f may run beside H2d/H2e | yes |
| 2026-11-14 | planning | Follow-up decisions: proprietary license (root `LICENSE` added; artifacts ship third-party notices); one semver in a root `VERSION` file from H2g, tagged on `main` in H6; installer unsigned for now; one agent serves several ESPs, one TCP connection per device chosen in the hello by alias or device id, lease per device, one token per agent. Step-level open items are listed in host-steps.md, "Left to the step plans" | User decisions after the host re-plan | yes |
| 2026-11-13 | F6, F10 | Test harnesses retry group-only Busy/StagingBusy rejections (in addition to Withdrawn), bounded to ten attempts; production SDK/CLI stays unchanged. F10 applies the same bounded retry to the native "autonomous loops" group test (`test_core_groups.cpp`) | CI's existing sub-enable scenario raced a loop's atomic slot claim (host-linux in F6; the native test on PR #60's CI, where the owning loop's `endTick` held the slot during `enable`). The rejection occurs before mutation; unit tests and independent review confirm timeouts/actions/expected refusals are not broadened | n/a |

## Hardware results

One row per run in [hardware-log.md](hardware-log.md). Firmware-era results are in [archive/firmware-status.md](archive/firmware-status.md#hardware-test-results).
