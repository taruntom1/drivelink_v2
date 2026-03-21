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
    cfg.update_task.enable = true;
    cfg.update_task.period_ms = 10;

    motor_manager::MotorManager manager(uart, cfg);
    if (!manager.isValid()) {
        return;
    }

    // Optional for manual scheduling:
    // while (true) {
    //     manager.updateAll();
    //     manager.publishTelemetry();
    //     vTaskDelay(pdMS_TO_TICKS(10));
    // }
}
```

## Binary protocol (version 1)

All payloads begin with:

- `version` (`uint8_t`) = `0x01`

### Configure command (single callback)

- UART message type: `0x21` (default)
- Callback: only one callback registered for this message type
- Payload format:
  - `version` (`uint8_t`)
  - `subcommand` (`uint8_t`) (`ConfigureSubcommand`)
  - `motor_id` (`uint8_t`)
  - subcommand-specific bytes

Subcommands:

- `0x01` CREATE_MOTOR
- `0x02` DELETE_MOTOR
- `0x03` CONFIGURE_MOTOR
- `0x04` SET_MODE
- `0x05` SET_PID_PARAMS
- `0x06` SET_SETPOINT
- `0x07` STOP_MOTOR
- `0x08` RESET_MOTOR

CREATE/CONFIGURE payload body (after first 3 bytes):

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
- `pos_kp` `float32`
- `pos_ki` `float32`
- `pos_kd` `float32`
- `velocity_feedforward` `float32`
- `position_feedforward` `float32`
- `reserved0` `float32` (for extensibility)
- `reserved1` `float32` (for extensibility)
- `sample_time_ms` `uint32_t`

SET_MODE body:

- `mode` `uint8_t`: 0=open-loop, 1=velocity, 2=position

SET_PID_PARAMS body:

- `pid_kind` `uint8_t`: 0=velocity, 1=position
- `kp` `float32`
- `ki` `float32`
- `kd` `float32`
- `sample_time_ms` `uint32_t`

SET_SETPOINT body:

- `setpoint_kind` `uint8_t`: 0=open-loop duty, 1=velocity rad/s, 2=position rad
- `value` `float32`

Configure response:

- UART message type: `0x61` (default)
- Payload:
  - `version` (`uint8_t`)
  - `subcommand` (`uint8_t`)
  - `motor_id` (`uint8_t`)
  - `result_code` (`uint8_t`)
  - `reserved` (`uint8_t`)

### Setpoint stream (PC → ESP)

- Channel: `0x80` (default)
- Payload:
  - `version` (`uint8_t`)
  - `entry_count` (`uint8_t`)
  - repeated entries:
    - `motor_id` (`uint8_t`)
    - `setpoint_kind` (`uint8_t`)
    - `value` (`float32`)

No per-packet dynamic allocation is used while parsing.

### Telemetry stream (ESP → PC)

- Channel: `0x81` (default)
- Payload:
  - `version` (`uint8_t`)
  - `entry_count` (`uint8_t`)
  - repeated entries:
    - `motor_id` (`uint8_t`)
    - `angle_rad` (`float32`)
    - `angular_velocity_rad_s` (`float32`)

Use `MotorManager::publishTelemetry()` periodically (or enable the internal update task).
