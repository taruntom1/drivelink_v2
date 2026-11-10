# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** F1a, the framing and codec extraction into `protocol/`.
- **Handoff:**
  - S1 moved the firmware into `firmware/` (commit 1 is moves only, so `git log --follow` works) and wrote the root `CLAUDE.md` (repo map, cross-cutting rules, the temporary overhaul section) and root `README.md`. `firmware/CLAUDE.md` and `firmware/components/CLAUDE.md` keep the firmware rules.
  - Paths in `firmware/CLAUDE.md` are relative to the repo root. Firmware commands run from the root with `-C firmware`. The nolog variant builds from inside `firmware/` (`idf.py -B build_variant -D SDKCONFIG=build_variant/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.nolog" build`), and CI does the same by `cd`ing into each app, because `-B` resolves against the current directory while `SDKCONFIG` resolves against the project.
  - The CI check for the HIL app is now named `idf.py build (firmware/test/uart_comm)`. If branch protection requires the old name, update it.
  - `gen_diag_codes.py` reads `firmware/components/diag/`. The code table is still `docs/protocol/diag-codes.md`.
  - F1a creates `protocol/` at the repo root. It joins the firmware build through `EXTRA_COMPONENT_DIRS` in `firmware/CMakeLists.txt` and `firmware/test/uart_comm/CMakeLists.txt`, and its tests join the host test build.
  - Local follow-ups for the user (not in the PR): delete the old root `build/` and `sdkconfig`, point the VS Code ESP-IDF extension at `firmware/`, and rebase other worktrees.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further. Then it adds rows here, for example F3a-1 and F3a-2.

| Step | Content | Hardware check | Build model · effort | Review effort | State | PR |
|---|---|---|---|---|---|---|
| S0 | Workflow: this file, the temporary `CLAUDE.md` section, CI on `firmware_overhaul`, the `/overhaul-step` skill, plan updates for the cleanup, the diag code table moved to `docs/protocol/` | no | Sonnet · medium | high | in review | [#28](https://github.com/taruntom1/drivelink_v2/pull/28) |
| S1 | Repo restructure: firmware into `firmware/` (host plan, "S1: restructure in detail") | no | Sonnet · medium | high | in review | PR_LINK |
| F1a | Extract framing, CRC, scanner and `wire_codec` into `protocol/` with the wire format unchanged; `uart_comm` fixes for big frames (buffers in `Impl`, stack sizes, non-blocking `sendData`) | no | Sonnet · high | high | planned | |
| F1b | New framing: u16 LEN + HCRC8, `MAX_PAYLOAD` 1024, 921600 baud default, UART ISR in IRAM, protocol version bumps, HIL tests and `conftest.py` baud | yes: HIL pytest | Opus · high | xhigh | planned | |
| F2 | `tools/gen/`, `protocol/defs/` (registry, diag codes), Kconfig MSG_TYPEs replaced, legacy streams moved to `0xBD–0xBF`, descriptor table format and hash | no | Sonnet · high | high | planned | |
| F3a | `core_os` (IDF and native), registry, loops, layout and NVS (A/B, validation, GPIO registry), safe mode, `firmware/test/native` | no | Opus · high | high | planned | |
| F3b | System, layout, describe, param and action families; status stream; `GET_INFO`; diag v2; `mod_mock` | yes: mock instances on the board | Sonnet · high | high | planned | |
| F3c | `drivelink-vdev`, first `libdrivelink` slice, `drivelink-cli`, `host/superbuild`, host-linux CI | no | Sonnet · high | high | planned | |
| F4a | Subscription and command groups (preallocated, epochs) | no | Sonnet · high | high | planned | |
| F4b | Arming, `DISARM` fast path, safety monitor, watchdog, `DIAG_RAISE`, `blocks_arm` / `disarms`, `kind: latched`; CLI arm and stream commands | yes: monitor and watchdog on the board with mock modules | Opus · high | xhigh | planned | |
| F5a | Motor module types (actuator, feedback, controller) with `emergencyStop`, `onArmTransition`, bumpless mode switch | yes: motors | Opus · high | xhigh | planned | |
| F5b | Delete `motor_manager` and the legacy motor streams; loop-stall test on hardware | yes: motors | Sonnet · high | xhigh | planned | |
| F6 | `imu.bno055` module and calibration actions; delete `imu_bmo_055` | yes: IMU | Sonnet · high | high | planned | |
| F7a | Capture family, PSRAM (caps alloc only) | yes: capture | Sonnet · high | high | planned | |
| F7b | Test signal generator; evaluate `SPIRAM_XIP_FROM_PSRAM` | yes: motors | Sonnet · high | high | planned | |
| F8 | Time ping with TX stamping; delete `time_sync` | yes: time jitter | Sonnet · high | high | planned | |
| F9 | OTA: partition table, OTA family, rollback | yes: OTA and rollback | Opus · high | xhigh | planned | |
| F10 | Cleanup: leftover Kconfig and legacy registry entries, rewrite `firmware/CLAUDE.md` for the new architecture, remove the temporary overhaul section from the root `CLAUDE.md`, merge `firmware_overhaul` into `main` | yes: full smoke test | Sonnet · medium | high | planned | |
| H1 | Complete `libdrivelink`: time model, device model, services, plant identification math; host-windows CI. May start in a second session after F4b is merged | no (vdev) | Opus · high | high | planned | |
| H2 | GUI in local mode (Qt/QML): connect, device, layout, tune and monitor screens | yes: bench tuning | Sonnet · high | high | planned | |
| H3 | Agent: arbitration, lease, clock model distribution; GUI remote mode | yes: Pi + PC | Opus · high | xhigh | planned | |
| H4 | `drivelink_hardware`, `drivelink_bringup`, diagnostics node, ROS CI | yes: robot | Opus · high | xhigh | planned | |

## Models and effort

- **Plan** every step with **Opus 5.5 at high effort**, whatever the step's build model. The plan sets the direction.
- **Build** with the step's model and effort from the table. After you approve the plan, switch the session model if the build model differs.
- **Review** always runs on **Opus**, at the step's review effort. `/overhaul-step` launches the review agent with that model and effort.
  - `xhigh` is for steps where a bug can drive a motor or brick the board: framing (F1b), arming and the monitor (F4b), motors (F5a, F5b), OTA (F9), the agent (H3) and ROS (H4).
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's tier.** If a step turns out harder (or easier) than its tier, change its row in the same PR and log the reason.

## Decision log

Choices made during implementation that the plans don't cover, or that change them. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-10 | S0 | The diag code table lives in `docs/protocol/diag-codes.md` | `components/diag/README.md` was removed in `f4de709`, which broke the generator check | n/a (already the plan's direction) |
| 2026-11-10 | S1 | CI builds each app from inside its directory (`cd`) instead of `idf.py -C <app> -B build_variant ...` | `-B` resolves against the current directory but `SDKCONFIG` and the defaults resolve against the project dir, so with `-C` the variant paths would split between the root and `firmware/` | n/a |
| 2026-11-10 | S1 | `firmware/CLAUDE.md` writes all paths relative to the repo root, and the root `CLAUDE.md` holds the repo map and cross-cutting rules | An agent never has to guess the base directory | n/a |
| 2026-11-10 | S0 | The restructure no longer moves `review/` or `TODO.md`, and phases update `docs/protocol/` instead of component READMEs or `review/` statuses | Those files were removed in `f4de709` | yes |

## Hardware test results

| Date | Step | Board | What was run | Result |
|---|---|---|---|---|

## Notes

- **Legacy wire specs.** The deleted READMEs (motor_manager v4, IMU v4, time_sync v1, diag v1) are in git history: `git show 7a4d693:components/<name>/README.md`. The IMU guide is still at `firmware/components/imu_bmo_055/docs/protocol-api-guide.md`.
- **diag's wire spec** has no page under `docs/protocol/` yet. F3b writes `docs/protocol/diag.md` when diag goes to v2.
