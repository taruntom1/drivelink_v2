# motor_manager

`motor_manager` manages multiple `motor_controller::MotorController` instances and exposes a single UART-facing control plane using `uart_comm`.

It is designed for ESP32-S3 and keeps UART callbacks short by queuing configuration work onto a dedicated task.

## Features

- Runtime add/remove motor instances by `motor_id`
- Runtime configuration updates (counts/rev, gear ratio, output limits, PID, feedforward)
- Runtime mode switching and setpoint updates
- One command callback for all configuration sub-commands
- One setpoint stream channel for all motors (PC → ESP)
- One telemetry stream channel for all motors (ESP → PC)
- Timer-driven control loop (esp_timer), independent of `CONFIG_FREERTOS_HZ`, with control and telemetry rates the host can change at runtime
- Thread-safe motor map access using FreeRTOS mutex

## Add to project

```cmake
idf_component_register(
    ...
    REQUIRES motor_manager
)
```

Include:

```cpp
#include "motor_manager.hpp"
```

## Basic usage

```cpp
#include "motor_manager.hpp"
#include "uart_comm.hpp"

extern "C" void app_main(void)
{
    uart_comm::UartComm uart({});
    if (!uart.isValid()) {
        return;
    }

    motor_manager::ManagerConfig cfg{};
    cfg.max_motors = 4;
    cfg.configure_msg_type = motor_manager::kManagerMsgConfigure;       // 0x21
    cfg.configure_response_msg_type = motor_manager::kManagerMsgConfigureResp; // 0x61
    cfg.setpoint_stream_channel = motor_manager::kManagerSetpointStreamChannel; // 0x80
    cfg.telemetry_stream_channel = motor_manager::kManagerTelemetryStreamChannel; // 0x81
    cfg.use_mock_feedback = true; // no motor hardware required
    cfg.update_task.enable = true;
    cfg.update_task.control_period_us = 1000;    // 1 kHz control loop
    cfg.update_task.telemetry_period_us = 10000; // telemetry at 100 Hz

    motor_manager::MotorManager manager(uart, cfg);
    if (!manager.isValid()) {
        return;
    }

    // Only with cfg.update_task.enable = false (manual scheduling):
    // while (true) {
    //     manager.updateAll(0.01f);  // dt in seconds
    //     manager.publishTelemetry();
    //     vTaskDelay(pdMS_TO_TICKS(10));
    // }
}
```

## Control loop and timing

With `update_task.enable = true` (the default), a periodic `esp_timer` wakes the update task every `control_period_us`. The loop does not use the FreeRTOS tick, so its rate does not depend on `CONFIG_FREERTOS_HZ`.

On each wake the update task:
1. Works out `dt = elapsed timer ticks × control period`. The tick count comes from the task notification count, so a late or skipped tick gives a longer `dt` instead of a wrong one. At most 10 ticks are folded into one `dt`.
2. Try-locks the manager mutex. If a configure command holds it, the tick is skipped and its time is added to the next run's `dt`.
3. Applies the setpoints queued from the stream, then calls `MotorController::update(dt)` for every motor. Each controller estimates velocity from the encoder count delta and passes the same `dt` to its PIDs.
4. Every `N = ceil(telemetry_period_us / control_period_us)` ticks, encodes a telemetry packet. Its timestamp is taken when the motors are sampled, and the packet is sent after the mutex is released.

Late and skipped ticks are counted (`getControlOverrunCount()`) and logged at most once per second. With `MOTOR_MANAGER_LOG_LOOP_STATS` enabled, the interval and run-time statistics are logged every 5 s.

The task is pinned to `update_task.core_id` (default core 1) at `update_task.priority` (default 20, below the esp_timer task's 22).

Both periods can be changed at runtime with `setTiming()` or the `SET_TIMING` subcommand:
- The control period is clamped to 250–100000 µs (`kControlPeriodMinUs` / `kControlPeriodMaxUs`).
- The telemetry period is clamped to [control period, 1 s] and rounded up to a whole number of control periods.
- `0` keeps the current value. The requested telemetry period is remembered, so changing only the control period keeps the telemetry rate as close as possible to what was asked.
- The new control period applies from the next tick. PID `dt` and all filter coefficients are derived from `dt`, so nothing else needs retuning.

## Mock feedback mode

For hardware-free host integration, enable mock feedback mode.

Behavior in this mode:

- No real `motor_controller::MotorController` instances are created.
- Incoming setpoints are stored per motor.
- Telemetry echoes stored setpoints as feedback values.
  - velocity setpoint and open-loop duty update telemetry velocity
  - position setpoint updates telemetry angle

Enable via either:

- `ManagerConfig::use_mock_feedback = true`
- Kconfig option `MOTOR_MANAGER_USE_MOCK_FEEDBACK`

## Binary protocol (version 3)

All payloads begin with:

- `version` (`uint8_t`) = `0x03`

Payloads with any other version are rejected.

Changes from version 2:
- CREATE/CONFIGURE: `sample_time_ms` and the two reserved floats are removed. `vel_d_cutoff_hz`, `pos_d_cutoff_hz` and `vel_filter_cutoff_hz` are added. The body is now 64 bytes.
- SET_PID_PARAMS: `sample_time_ms` is replaced by `d_cutoff_hz`.
- New SET_TIMING subcommand (`0x0B`). Its response carries a body after the 5-byte header.
- Configure bodies must have their exact size. Before, extra trailing bytes were ignored.
- Telemetry has a new field: bit2, the applied output duty.
- The setpoint stream layout is unchanged.

The PID `dt` now always comes from the control loop timing (see [Control loop and timing](#control-loop-and-timing)), so the host no longer sends a sample time.

Code layout: the wire-level enums, field values and sizes, and `MotorConfigPayload` (the CreateMotor / ConfigureMotor body, with `uint8_t` GPIO numbers) are in the pure public header `include/motor_manager_protocol.hpp`. The codec that turns payloads into requests and back (`priv_include/motor_protocol.hpp`, `src/motor_protocol.cpp`) has no ESP-IDF dependency and is covered by the host unit tests in `test/host/`. The MSG_TYPEs and stream channels come from Kconfig and are in `motor_manager_types.hpp`.

### Configure command (single callback)

- UART message type: `CONFIG_MOTOR_MANAGER_CONFIGURE_MSG_TYPE` (default `0x21`)
- Callback: only one callback registered for this message type
- Payload format:
  - `version` (`uint8_t`)
  - `seq` (`uint8_t`): chosen by the host and echoed in the response
  - `subcommand` (`uint8_t`) (`ConfigureSubcommand`)
  - `motor_id` (`uint8_t`)
  - subcommand-specific bytes

Commands are queued (up to `MOTOR_MANAGER_CONFIG_QUEUE_DEPTH`, default 8) and answered in order. Give every in-flight command a different `seq` (for example a wrapping counter) and match each response on `seq`; `subcommand` and `motor_id` alone cannot tell two commands to the same motor apart.

Every subcommand body has a fixed size, and the body must be exactly that size. A body with missing or extra bytes gets `INVALID_ARGUMENT`. DELETE_MOTOR, STOP_MOTOR and RESET_MOTOR have an empty body. All multi-byte values are little-endian.

Invalid values are rejected with `INVALID_ARGUMENT`, and nothing is applied:
- any non-finite `float32` (NaN, ±Inf) in any body
- an unknown `mode`, `pid_kind` or `setpoint_kind`
- telemetry `field_mask` bits outside `0x07`
- CREATE_MOTOR pins (when not in mock mode) that are not four distinct GPIOs a host may use: they must exist on the ESP32-S3 (0–21, 26–48), and must not be USB (19, 20), flash/PSRAM (26–37), the uart_comm TX/RX pins, or a pin another motor already owns. CONFIGURE_MOTOR does not change pins, so it ignores them.

Valid numbers outside their range are clamped or ignored as before (for example `output_min`/`output_max` are clamped to [-1, 1], and a non-positive `counts_per_rev` keeps the previous value).

Subcommands:

- `0x01` CREATE_MOTOR
- `0x02` DELETE_MOTOR
- `0x03` CONFIGURE_MOTOR
- `0x04` SET_MODE
- `0x05` SET_PID_PARAMS
- `0x06` SET_SETPOINT
- `0x07` STOP_MOTOR
- `0x08` RESET_MOTOR
- `0x09` SET_TELEMETRY_GLOBAL
- `0x0A` SET_TELEMETRY_MOTOR
- `0x0B` SET_TIMING

CREATE/CONFIGURE payload body (after the 4 header bytes, 64 bytes, so 68 bytes in total):

- `pwm_gpio` `uint8_t`
- `dir_gpio` `uint8_t`
- `enc_a` `uint8_t`
- `enc_b` `uint8_t`
- `counts_per_rev` `float32`
- `gear_ratio` `float32`
- `output_min` `float32`
- `output_max` `float32`
- `vel_kp` `float32`
- `vel_ki` `float32`
- `vel_kd` `float32`
- `vel_d_cutoff_hz` `float32`: velocity PID derivative filter cutoff, `0` = unfiltered
- `pos_kp` `float32`
- `pos_ki` `float32`
- `pos_kd` `float32`
- `pos_d_cutoff_hz` `float32`: position PID derivative filter cutoff, `0` = unfiltered
- `velocity_feedforward` `float32`
- `position_feedforward` `float32`
- `vel_filter_cutoff_hz` `float32`: low-pass cutoff of the velocity estimate, `0` = unfiltered

The cutoffs are frequencies, not per-sample coefficients, so they behave the same at any control rate. A negative cutoff disables that filter at creation and is ignored by CONFIGURE.

SET_MODE body:

- `mode` `uint8_t`: 0=open-loop, 1=velocity, 2=position

SET_PID_PARAMS body:

- `pid_kind` `uint8_t`: 0=velocity, 1=position
- `kp` `float32`
- `ki` `float32`
- `kd` `float32`
- `d_cutoff_hz` `float32`: derivative filter cutoff, `0` = unfiltered

SET_SETPOINT body:

- `setpoint_kind` `uint8_t`: 0=open-loop duty, 1=velocity rad/s, 2=position rad
- `value` `float32`

SET_TELEMETRY_GLOBAL body:

- `enabled` `uint8_t`: `0` disable all telemetry, non-zero enable telemetry output
- `field_mask` `uint8_t`:
  - bit0 (`0x01`) angle (`angle_rad`)
  - bit1 (`0x02`) velocity (`angular_velocity_rad_s`)
  - bit2 (`0x04`) output (`output_duty`)

SET_TELEMETRY_MOTOR body:

- `enabled` `uint8_t`: `0` disable telemetry for this motor, non-zero enable
- `field_mask` `uint8_t`: same bit mapping as global `field_mask`
- `apply_field_mask` `uint8_t`: `0` keep previous per-motor field mask, non-zero update it from `field_mask`

SET_TIMING body (global; `motor_id` is ignored and echoed back):

- `control_period_us` `uint32_t`: control loop period, `0` = keep current
- `telemetry_period_us` `uint32_t`: telemetry period, `0` = keep current

The firmware clamps and rounds both values (see [Control loop and timing](#control-loop-and-timing)). Send `0, 0` to read the current values without changing them.

Configure response:

- UART message type: `CONFIG_MOTOR_MANAGER_CONFIGURE_RESPONSE_MSG_TYPE` (default `0x61`)
- Payload (5 bytes, plus a body for some subcommands):
  - `version` (`uint8_t`)
  - `seq` (`uint8_t`): echoed from the request
  - `subcommand` (`uint8_t`): echoed from the request
  - `motor_id` (`uint8_t`): echoed from the request
  - `result_code` (`uint8_t`)
  - response body, only for SET_TIMING with `result_code = OK` (8 bytes):
    - `applied_control_period_us` `uint32_t`
    - `applied_telemetry_period_us` `uint32_t`

If the request header could not be parsed (payload shorter than 4 bytes, or wrong `version`), the response has `subcommand = 0xFF`, `motor_id = 0xFF` and `result_code = INVALID_ARGUMENT`. `seq` is echoed when the request's version was right and it carried a `seq` byte; otherwise it is `0`.

### Setpoint stream (PC → ESP)

- Channel: `CONFIG_MOTOR_MANAGER_SETPOINT_STREAM_CHANNEL` (default `0x80`)
- Payload:
  - `version` (`uint8_t`)
  - `entry_count` (`uint8_t`)
  - repeated entries:
    - `motor_id` (`uint8_t`)
    - `setpoint_kind` (`uint8_t`)
    - `value` (`float32`)

The payload length must be exactly `2 + entry_count * 6`. A frame with missing or extra bytes is dropped as a whole.

No per-packet dynamic allocation is used while parsing.

When the update task is enabled, the UART RX task does not apply setpoints itself. It stores the latest value per motor and setpoint kind in a mailbox, and the update task applies them at the start of its next control tick (within one control period). If several frames arrive between ticks, only the newest value for each (motor, kind) is applied. When the update task is disabled, setpoints are applied straight away on the RX task.

Each entry is checked on the RX task before it is queued: the motor id must exist, the kind must be 0–2 and the value finite. Setpoints have no response. Dropped frames (wrong version or length) and rejected entries are counted, not logged one by one: neither the RX task nor the control tick logs. `getSetpointStreamCounters()` returns the totals, and the config task logs a summary of new counts at most once per second.

### Telemetry stream (ESP → PC)

- Channel: `CONFIG_MOTOR_MANAGER_TELEMETRY_STREAM_CHANNEL` (default `0x81`)
- Payload:
  - `version` (`uint8_t`)
  - `entry_count` (`uint8_t`)
  - `timestamp_us` (`uint64_t`, from `gettimeofday()`, so host-synced once `TimeSync` has run; shared by all entries in the packet). It is taken in the control tick that sampled the motors, so consecutive packets are spaced by whole control periods.
  - repeated entries:
    - `motor_id` (`uint8_t`)
    - `field_mask` (`uint8_t`)
    - optional fields in this order, present when the corresponding `field_mask` bit is set:
      - bit0: `angle_rad` (`float32`)
      - bit1: `angular_velocity_rad_s` (`float32`), filtered estimate
      - bit2: `output_duty` (`float32`), duty applied to the motor in [-1, 1] (`0` in mock mode)

Effective per-entry field selection is:

- `effective_field_mask = global_field_mask & motor_field_mask`

If telemetry is globally disabled, no telemetry packet is sent.

With the internal update task, telemetry is sent every N control ticks (see [Control loop and timing](#control-loop-and-timing)). Without it, call `MotorManager::publishTelemetry()` yourself.

## Kconfig

Under `Motor Manager Configuration` in `menuconfig`, these defaults can be changed without code edits:

- `MOTOR_MANAGER_CONFIGURE_MSG_TYPE`
- `MOTOR_MANAGER_CONFIGURE_RESPONSE_MSG_TYPE`
- `MOTOR_MANAGER_SETPOINT_STREAM_CHANNEL`
- `MOTOR_MANAGER_TELEMETRY_STREAM_CHANNEL`
- `MOTOR_MANAGER_USE_MOCK_FEEDBACK`
- `MOTOR_MANAGER_DEFAULT_UPDATE_TASK_STACK`, `MOTOR_MANAGER_DEFAULT_UPDATE_TASK_PRIO` (default 20), `MOTOR_MANAGER_DEFAULT_CONTROL_TASK_CORE` (default 1, `-1` = no affinity)
- `MOTOR_MANAGER_DEFAULT_CONTROL_PERIOD_US` (default 1000)
- `MOTOR_MANAGER_DEFAULT_TELEMETRY_PERIOD_US` (default 10000)
- `MOTOR_MANAGER_LOG_LOOP_STATS` (default off)
- `MOTOR_MANAGER_DEFAULT_CONFIG_TASK_STACK`, `MOTOR_MANAGER_DEFAULT_CONFIG_TASK_PRIO` (default 6)
- `MOTOR_MANAGER_CONFIG_QUEUE_DEPTH` (default 8): configure frames queued for the config task
- `MOTOR_MANAGER_MUTEX_TIMEOUT_MS` (default 50): how long configure commands and the manual `updateAll()` / `publishTelemetry()` wait for the manager mutex. The control tick never waits.
