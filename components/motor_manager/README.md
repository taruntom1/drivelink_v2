# motor_manager

## Overview
`motor_manager` multiplexes control of multiple motor instances behind one UART protocol endpoint. It owns motor slot lifecycle, setpoint ingestion, configure command handling, and telemetry publication.

## Features
- Runtime create/delete/configure by motor ID
- Setpoint stream ingestion for multiple motors
- Global and per-motor telemetry masking
- Optional mock-feedback mode for hardware-free integration
- Dedicated configure task and optional update/telemetry periodic task

## Dependencies
Required:
- `motor_controller`
- `uart_comm`
- `freertos`
- `log`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_MOTOR_MANAGER_CONFIGURE_MSG_TYPE` | hex | `0x21` | Configure command message type. |
| `CONFIG_MOTOR_MANAGER_CONFIGURE_RESPONSE_MSG_TYPE` | hex | `0x61` | Configure response message type. |
| `CONFIG_MOTOR_MANAGER_SETPOINT_STREAM_CHANNEL` | hex | `0x80` | Setpoint stream channel. |
| `CONFIG_MOTOR_MANAGER_TELEMETRY_STREAM_CHANNEL` | hex | `0x81` | Telemetry stream channel. |
| `CONFIG_MOTOR_MANAGER_USE_MOCK_FEEDBACK` | bool | `n` | Enable mock telemetry mode. |
| `CONFIG_MOTOR_MANAGER_DEFAULT_UPDATE_TASK_STACK` | int | `4096` | Default update task stack size. |
| `CONFIG_MOTOR_MANAGER_DEFAULT_UPDATE_TASK_PRIO` | int | `6` | Default update task priority. |
| `CONFIG_MOTOR_MANAGER_DEFAULT_CONFIG_TASK_STACK` | int | `4096` | Default config task stack size. |
| `CONFIG_MOTOR_MANAGER_DEFAULT_CONFIG_TASK_PRIO` | int | `6` | Default config task priority. |

## Public API
Public headers:
- `include/motor_manager.hpp`
- `include/motor_manager_types.hpp`

Key methods:
- Lifecycle: constructor, destructor, `isValid()`
- Motor lifecycle/config: `createMotor`, `deleteMotor`, `configureMotor`
- Runtime control: `setMode`, `setPidParams`, `setSetpoint`, `stopMotor`, `resetMotor`
- Telemetry control: `setTelemetryGlobal`, `setTelemetryForMotor`
- Scheduling/hooks: `updateAll`, `publishTelemetry`, `handleSetpointStream`, `handleConfigureCommand`

Protocol enums/types include `ResultCode`, `ConfigureSubcommand`, `PidKind`, `SetpointKind`, `MotorConfigPayload`, `ManagerConfig`.

## Usage Example
```cpp
#include "motor_manager.hpp"
#include "uart_comm.hpp"

extern "C" void app_main(void)
{
    uart_comm::UartComm uart({});
    if (!uart.isValid()) {
        return;
    }

    motor_manager::MotorManager mgr(uart, {});
    if (!mgr.isValid()) {
        return;
    }
}
```

## Error Handling
`ResultCode` values:
- `Ok`, `InvalidArgument`, `NotFound`, `AlreadyExists`, `CapacityReached`, `InvalidState`, `NotSupported`, `InternalError`

Helper conversion API: `resultCodeToEspErr()`.

## Thread Safety
- Shared slot/telemetry state is protected by a mutex.
- Valid/stop flags are atomic.
- Configure callback enqueues frames for task-level processing.

## Limitations & Known Issues
- Max motors is bounded by `ManagerConfig::max_motors` and downstream HAL resources.
- Configure queue depth is fixed internally (`kConfigQueueDepth=8`) in current implementation.
