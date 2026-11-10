# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** S1, the repo restructure.
- **Handoff:**
  - S0 moved the diag code table from the deleted `components/diag/README.md` to `docs/protocol/diag-codes.md`, which repaired `gen_diag_codes.py --check`.
  - The cleanup commit `f4de709` removed the component READMEs, `review/`, `TODO.md` and the old plans. S1 must not try to move them. The host plan's restructure section is updated to match.
  - S1 moves `CLAUDE.md` to `firmware/CLAUDE.md` and writes a new root `CLAUDE.md`. The temporary overhaul section goes into the **root** file.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further. Then it adds rows here, for example F3a-1 and F3a-2.

| Step | Content | Hardware check | State | PR |
|---|---|---|---|---|
| S0 | Workflow: this file, the temporary `CLAUDE.md` section, CI on `firmware_overhaul`, the `/overhaul-step` skill, plan updates for the cleanup, the diag code table moved to `docs/protocol/` | no | in review | [#28](https://github.com/taruntom1/drivelink_v2/pull/28) |
| S1 | Repo restructure: firmware into `firmware/` (host plan, "Phase 1: restructure in detail") | no | planned | |
| F1a | Extract framing, CRC, scanner and `wire_codec` into `protocol/` with the wire format unchanged; `uart_comm` fixes for big frames (buffers in `Impl`, stack sizes, non-blocking `sendData`) | no | planned | |
| F1b | New framing: u16 LEN + HCRC8, `MAX_PAYLOAD` 1024, 921600 baud default, UART ISR in IRAM, protocol version bumps, HIL tests and `conftest.py` baud | yes: HIL pytest | planned | |
| F2 | `tools/gen/`, `protocol/defs/` (registry, diag codes), Kconfig MSG_TYPEs replaced, legacy streams moved to `0xBD–0xBF`, descriptor table format and hash | no | planned | |
| F3a | `core_os` (IDF and native), registry, loops, layout and NVS (A/B, validation, GPIO registry), safe mode, `firmware/test/native` | no | planned | |
| F3b | System, layout, describe, param and action families; status stream; `GET_INFO`; diag v2; `mod_mock` | yes: mock instances on the board | planned | |
| F3c | `drivelink-vdev`, first `libdrivelink` slice, `drivelink-cli`, `host/superbuild`, host-linux CI | no | planned | |
| F4a | Subscription and command groups (preallocated, epochs) | no | planned | |
| F4b | Arming, `DISARM` fast path, safety monitor, watchdog, `DIAG_RAISE`, `blocks_arm` / `disarms`, `kind: latched`; CLI arm and stream commands | yes: monitor and watchdog on the board with mock modules | planned | |
| F5a | Motor module types (actuator, feedback, controller) with `emergencyStop`, `onArmTransition`, bumpless mode switch | yes: motors | planned | |
| F5b | Delete `motor_manager` and the legacy motor streams; loop-stall test on hardware | yes: motors | planned | |
| F6 | `imu.bno055` module and calibration actions; delete `imu_bmo_055` | yes: IMU | planned | |
| F7a | Capture family, PSRAM (caps alloc only) | yes: capture | planned | |
| F7b | Test signal generator; evaluate `SPIRAM_XIP_FROM_PSRAM` | yes: motors | planned | |
| F8 | Time ping with TX stamping; delete `time_sync` | yes: time jitter | planned | |
| F9 | OTA: partition table, OTA family, rollback | yes: OTA and rollback | planned | |
| F10 | Cleanup: leftover Kconfig and legacy registry entries, rewrite `CLAUDE.md` for the new architecture, remove the temporary overhaul section, merge `firmware_overhaul` into `main` | yes: full smoke test | planned | |
| H1–H4 | Host phases (host plan). H1 may start in a second session after F4b is merged | per phase | planned | |

## Decision log

Choices made during implementation that the plans don't cover, or that change them. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-10 | S0 | The diag code table lives in `docs/protocol/diag-codes.md` | `components/diag/README.md` was removed in `f4de709`, which broke the generator check | n/a (already the plan's direction) |
| 2026-11-10 | S0 | The restructure no longer moves `review/` or `TODO.md`, and phases update `docs/protocol/` instead of component READMEs or `review/` statuses | Those files were removed in `f4de709` | yes |

## Hardware test results

| Date | Step | Board | What was run | Result |
|---|---|---|---|---|

## Notes

- **Legacy wire specs.** The deleted READMEs (motor_manager v4, IMU v4, time_sync v1, diag v1) are in git history: `git show 7a4d693:components/<name>/README.md`. The IMU guide is still at `components/imu_bmo_055/docs/protocol-api-guide.md`.
- **diag's wire spec** has no page under `docs/protocol/` yet. F3b writes `docs/protocol/diag.md` when diag goes to v2.
