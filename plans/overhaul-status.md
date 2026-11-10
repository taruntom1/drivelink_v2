# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** F1b, the new framing (u16 LEN + HCRC8, `MAX_PAYLOAD` 1024, 921600 baud, UART ISR in IRAM, protocol version bumps). It needs the user's board for the HIL run.
- **Handoff (F1a):**
  - `protocol/` is at the repo root: an IDF component through `EXTRA_COMPONENT_DIRS` (in `firmware/CMakeLists.txt` and `firmware/test/uart_comm/CMakeLists.txt`) and a plain static library `drivelink::protocol` elsewhere. Namespace `drivelink::protocol`, headers under `drivelink/protocol/`: `wire_codec.hpp` (`::wire`), `crc.hpp`, `framing.hpp`, `scanner.hpp`. Its tests are in `protocol/test/` and are compiled into `firmware/test/host` (it has the vendored doctest). The wire format is unchanged. The CRC is now a pure table (the ROM CRC is gone), cross-checked against `firmware/test/uart_comm/protocol.py`.
  - F1b changes the frame in `framing.hpp` (`kMaxPayload`, `encodeFrame`, `validateCrc`, a header CRC8) and in `scanner.cpp` (reject on HCRC or LEN before waiting for the payload). `Scanner` takes `max_payload` and sizes its window as two frames. The `uint8_t` LEN assumption sits in `framing.cpp`, `scanner.cpp` and the legacy managers' outbound sizes: the telemetry `uint8_t payload_len` / `encodeTelemetryLocked(..., uint8_t &)` in `motor_manager.cpp`, the IMU's `uint8_t response_len` and `response_extra_len`, and `motor_manager`'s `static_cast<uint8_t>(payload_len)` in `sendConfigureResponse`. Widen them in F1b if the managers' frames can pass 255 bytes. `RxFrame::len` is already `uint16_t`, `sendCommand` / `sendData` already take `size_t`, and the managers' inbound configure parsing was widened to `size_t` in F1a.
  - `uart_comm` now allocates everything at setup: a command buffer pool (`CONFIG_UART_COMM_CMD_QUEUE_DEPTH` buffers of one max frame, two index queues), `Impl::rx_frame`, the TX stream encode buffer, and per-channel stream slots from `openStream(channel, max_len)`. `sendData` on an unopened channel fails with `ESP_ERR_INVALID_STATE`; a busy slot skips the write (`ESP_ERR_TIMEOUT`, counted in `ErrorCounters::stream_skipped`). `motor_manager` and the IMU open their channel in their constructors (new `InitFailed` stage 9 and 10 in `diag_codes.yaml`).
  - **Raising `MAX_PAYLOAD` to 1024 costs memory in F1b:** the command pool grows to `depth × 1033` bytes, `RxFrame` to about 1 KB (it is copied into `rx_queue` and into the managers' config queues, whose item size is `sizeof(RxFrame)`), and the stack buffers `uint8_t payload[CONFIG_UART_COMM_MAX_PAYLOAD]` in `motor_manager.cpp` (telemetry encode, two places) and `imu_bmo_055.cpp` (`tx_payload`) become 1 KB on the stack. Check each, and the `static_assert`s in `diag_reporter.cpp`. `CONFIG_UART_COMM_TX_BUF_SIZE` (1024) should exceed a full frame, or `uart_write_bytes` blocks the TX task. The Kconfig range for `UART_COMM_MAX_PAYLOAD` is still 1..255.
  - Task stacks are unchanged (4096 each). `UartComm::stackHighWater()` (backed by the new `StoppableTask::stackHighWaterMark()`) is logged by the HIL test app on every idle timeout. F1b sets the sizes from the marks it reads with 1024-byte frames.
  - No hardware was run in F1a. F1b's HIL run is the first hardware test of the rewritten TX paths, so expect to look at `stream_skipped` and the stack marks there.
  - Local follow-ups for the user (not in the PR): delete the old root `build/` and `sdkconfig`, point the VS Code ESP-IDF extension at `firmware/`, rebase other worktrees, and update branch protection if it requires the old name of the HIL CI check (now `idf.py build (firmware/test/uart_comm)`).

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further. Then it adds rows here, for example F3a-1 and F3a-2.

| Step | Content | Hardware check | Build model · effort | Review effort | State | PR |
|---|---|---|---|---|---|---|
| S0 | Workflow: this file, the temporary `CLAUDE.md` section, CI on `firmware_overhaul`, the `/overhaul-step` skill, plan updates for the cleanup, the diag code table moved to `docs/protocol/` | no | Sonnet · medium | high | merged | [#28](https://github.com/taruntom1/drivelink_v2/pull/28) |
| S1 | Repo restructure: firmware into `firmware/` (host plan, "S1: restructure in detail") | no | Sonnet · medium | high | merged | [#29](https://github.com/taruntom1/drivelink_v2/pull/29) |
| F1a | Extract framing, CRC, scanner and `wire_codec` into `protocol/` with the wire format unchanged; `uart_comm` fixes for big frames (buffers in `Impl`, stack checks, non-blocking `sendData`) | no | Sonnet · high | high | in review | |
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
| 2026-11-10 | F1a | `protocol/` uses namespace `drivelink::protocol` (`::wire` for the codec) and headers under `drivelink/protocol/` | Matches the host plan's tree and keeps the host from depending on `uart_comm` | n/a |
| 2026-11-10 | F1a | CRC-16 is a pure table in `protocol/` instead of `esp_rom_crc16_be` | One tested implementation for the firmware and the host; same results (check value 0x29B1, checked against `protocol.py`) | n/a |
| 2026-11-10 | F1a | `Scanner` is the pure half of the old `Parser`: it returns frames as views, has no clock and no logging, and loops past rejected candidates itself. `Parser` keeps the driver reads, the stale timer, the atomics and the log lines | `protocol/` has to build without IDF or an OS | n/a |
| 2026-11-10 | F1a | Task stack sizes are not changed in F1a; `stackHighWater()` is added and logged, and F1b sets the sizes from HIL measurements | With 255-byte frames and the buffers moved off the stacks, nothing needs a bigger stack yet; the real sizes depend on 1024-byte frames | n/a |
| 2026-11-10 | F1a | `sendData` needs `openStream(channel, max_len)` first, and a busy slot skips the write (counted locally in `ErrorCounters::stream_skipped`, no diag code until F3b) | The control tick must never wait or allocate; each channel gets a slot of its own size | n/a |
| 2026-11-10 | F1a | The CMake package export and `package.xml` for `protocol/` wait for F3c; the protocol tests are built by `firmware/test/host` until then | No host consumer exists yet | n/a |

## Hardware test results

| Date | Step | Board | What was run | Result |
|---|---|---|---|---|

## Notes

- **Legacy wire specs.** The deleted READMEs (motor_manager v4, IMU v4, time_sync v1, diag v1) are in git history: `git show 7a4d693:components/<name>/README.md`. The IMU guide is still at `firmware/components/imu_bmo_055/docs/protocol-api-guide.md`.
- **diag's wire spec** has no page under `docs/protocol/` yet. F3b writes `docs/protocol/diag.md` when diag goes to v2.
