# DriveLink v2 Firmware: Full-Repo Code Review

**Date:** 2026-11-08
**Scope:** Every first-party source file in the repo: `main/`, all 8 components under `components/`, and `test/uart_comm/`. The vendor Bosch driver (`bno055.c` / `bno055.h`) was only checked where our code calls it. The uncommitted change to `components/imu_bmo_055/CMakeLists.txt` is reviewed too.
**Branch:** `claude/repo-code-review-b94577` (HEAD `ded0a7d`)

## Files in this folder

| File | Aspect |
|------|--------|
| [01-concurrency-and-lifecycle.md](01-concurrency-and-lifecycle.md) | FreeRTOS task start/stop, races, use-after-free on teardown |
| [02-uart-protocol-and-framing.md](02-uart-protocol-and-framing.md) | `uart_comm` parser and framing, plus the message protocols built on it |
| [03-motor-control.md](03-motor-control.md) | PID, encoder, motor HAL, motor_manager behaviour and safety |
| [04-imu-bno055.md](04-imu-bno055.md) | BNO055 driver wrapper, IMU manager, the uncommitted CMake change |
| [05-time-sync-and-timestamps.md](05-time-sync-and-timestamps.md) | Time sync algorithm, timestamp time bases across streams |
| [06-input-validation-and-error-handling.md](06-input-validation-and-error-handling.md) | Untrusted host input, unchecked return codes, silent failures |
| [07-code-quality-and-simplification.md](07-code-quality-and-simplification.md) | Duplication, dead code, simpler forms |
| [08-performance-and-efficiency.md](08-performance-and-efficiency.md) | Hot paths, wasted work, memory |
| [09-testing.md](09-testing.md) | Current test harness and coverage gaps |
| [10-docs-and-conventions.md](10-docs-and-conventions.md) | Drift between code and docs, and deviations from the `CLAUDE.md` conventions |

## Severity legend

- **High**: can crash, corrupt memory, lose control frames under normal use, or drive hardware wrongly.
- **Medium**: wrong results or degraded behaviour in realistic conditions.
- **Low**: robustness, maintainability or doc issues.

## Top findings (ranked)

| # | Sev | Finding | Location |
|---|-----|---------|----------|
| 1 | High | **Fixed** (see [02](02-uart-protocol-and-framing.md) P1). `UART_DATA` events call `uart_flush_input()`, which deletes frames already sitting in the ring buffer. Frames sent back-to-back are dropped whenever the RX task falls behind. | [parser.cpp:53-56](../components/uart_comm/src/parser.cpp) |
| 2 | High | **Fixed** (see [01](01-concurrency-and-lifecycle.md) C1–C3). Teardown calls `vTaskDelete(handle)` on tasks that already deleted themselves. That is a double delete / use-after-free on the TCB. Affects MotorManager, ImuBno055Manager and TimeSync. | [motor_manager.cpp:663-674](../components/motor_manager/src/motor_manager.cpp), [imu_bmo_055.cpp:712-735](../components/imu_bmo_055/src/imu_bmo_055.cpp), [time_sync.cpp:235-256](../components/time_sync/time_sync.cpp) |
| 3 | High | `main.cpp` ships with `use_mock_feedback = true`. Motor commands are accepted and acknowledged, but no motor is ever driven and the telemetry is fake. | [main.cpp:26](../main/main.cpp) |
| 4 | Medium | **Fixed** (see [03](03-motor-control.md) M2–M3: timer-driven loop, explicit `dt`, per-tick velocity estimate). The PID uses the configured `sample_time_ms` as `dt`. The loop runs on its own fixed 10 ms period and the encoder rate only refreshes every 50 ms. The I and D terms are mis-scaled and see stale measurements. | [pid_controller.cpp:160](../components/pid/src/pid_controller.cpp), [encoder.cpp:30-34](../components/rotary_encoder_hal/src/encoder.cpp) |
| 5 | Medium | BNO055 calibration offsets are read without switching to CONFIG mode first, so `ReadCalibration` returns stale or invalid data in fusion modes. | [bno055_device.cpp:267-321](../components/imu_bmo_055/src/bno055_device.cpp) |
| 6 | Medium | **Partly fixed** (README corrected; the time bases still differ, see [05](05-time-sync-and-timestamps.md) T1). Motor telemetry stamps wall-clock time (`gettimeofday`, stepped by time_sync), but the README says "since boot". IMU telemetry uses `esp_timer` since boot. The two streams cannot be correlated. | [motor_manager.cpp:593-596](../components/motor_manager/src/motor_manager.cpp), [bno055_device.cpp:175](../components/imu_bmo_055/src/bno055_device.cpp) |
| 7 | Medium | The IMU silently falls back to *random* mock data when the sensor is missing (Kconfig default `y`). The stream packet has no source flag, so the host cannot tell. | [imu_bmo_055.cpp:315-318](../components/imu_bmo_055/src/imu_bmo_055.cpp) |
| 8 | Medium | GPIO numbers from the host go straight into `gpio_config` / MCPWM / PCNT. `1ULL << dir_gpio` is undefined behaviour for values ≥ 64. Flash, PSRAM and UART pins can be reconfigured. | [motor_manager_uart.cpp:145-148](../components/motor_manager/src/motor_manager_uart.cpp), [esp_motor.cpp:30](../components/motor_hal/src/esp_motor.cpp) |
| 9 | Medium | **Fixed** (see [02](02-uart-protocol-and-framing.md) P2: software scan with one-byte resync). The 3×`0xAA` start pattern is not escaped and is detected with zero idle gaps, so payload or CRC bytes can create false frame starts. Recovery relies on CRC and on flushes that lose data. | [uart_comm.cpp:203-204](../components/uart_comm/src/uart_comm.cpp) |
| 10 | Low | The uncommitted CMake change compiles Bosch's `bno055_support.c` template into the firmware. It adds a global `bno055` and a dead `bno055_data_readout_template()` that would re-point the driver at a struct with null bus callbacks. | [CMakeLists.txt:2](../components/imu_bmo_055/CMakeLists.txt), [bno055_support.c:103,319](../components/imu_bmo_055/bno055_support.c) |

## What is in good shape

- The component layout (`include/` / `priv_include/` / `src/`, Kconfig, explicit `REQUIRES`) is consistent and matches the project guidelines.
- Peripheral pools (MCPWM, PCNT) are centralised, mutex-protected, and unwind failures carefully step by step.
- `uart_comm` cleanly separates command/response traffic (FIFO queue, priority drain) from stream traffic (latest-wins slots).
- Protocol payload parsers check lengths on every read, using helpers that check bounds (`readFloatLe`, `readU16Le`).
- Configure commands are moved off the RX task onto dedicated config tasks through queues. This is the right shape. The setpoint stream callback is the exception (see 02).
- The `uart_comm` hardware test suite exists and covers CRC, echo, stream and counter behaviour.
