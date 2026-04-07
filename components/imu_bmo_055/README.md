# imu_bmo_055

## Overview
`imu_bmo_055` manages a Bosch BNO055 IMU over I2C and exposes runtime configuration and telemetry over `uart_comm`. It runs a command-processing task and a periodic stream task, with optional mock-data fallback when hardware is unavailable.

## Features
- BNO055 initialization and operation-mode control
- Runtime configure protocol over UART command message type
- Periodic telemetry streaming with selectable field mask
- Calibration read/write/apply command set
- Optional mock telemetry mode and fallback-on-init-failure mode

## Dependencies
Required:
- `uart_comm`
- `driver`
- `freertos`
- `log`
- `esp_timer`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_IMU_BMO_055_CONFIGURE_MSG_TYPE` | hex | `0x22` | Configure request message type. |
| `CONFIG_IMU_BMO_055_CONFIGURE_RESPONSE_MSG_TYPE` | hex | `0x42` | Configure response message type. |
| `CONFIG_IMU_BMO_055_EVENT_MSG_TYPE` | hex | `0xC2` | Reserved event message type. |
| `CONFIG_IMU_BMO_055_STREAM_CHANNEL` | hex | `0x82` | Telemetry stream channel. |
| `CONFIG_IMU_BMO_055_USE_MOCK_DATA` | bool | `n` | Force synthesized IMU data source. |
| `CONFIG_IMU_BMO_055_FALLBACK_TO_MOCK_ON_INIT_FAIL` | bool | `y` | Enter mock mode if sensor init fails. |
| `CONFIG_IMU_BMO_055_I2C_PORT_NUM` | int | `0` | I2C peripheral index. |
| `CONFIG_IMU_BMO_055_I2C_SDA_PIN` | int | `8` | I2C SDA pin. |
| `CONFIG_IMU_BMO_055_I2C_SCL_PIN` | int | `9` | I2C SCL pin. |
| `CONFIG_IMU_BMO_055_I2C_CLK_HZ` | int | `400000` | I2C bus clock. |
| `CONFIG_IMU_BMO_055_I2C_TIMEOUT_MS` | int | `50` | I2C transaction timeout. |
| `CONFIG_IMU_BMO_055_DEVICE_ADDRESS` | hex | `0x28` | BNO055 address. |
| `CONFIG_IMU_BMO_055_DEFAULT_OPERATION_MODE` | hex | `0x0C` | Startup operation mode. |
| `CONFIG_IMU_BMO_055_DEFAULT_STREAM_FIELD_MASK` | hex | `0x1FF` | Startup telemetry field mask. |
| `CONFIG_IMU_BMO_055_DEFAULT_STREAM_PERIOD_MS` | int | `10` | Startup stream period. |
| `CONFIG_IMU_BMO_055_MIN_STREAM_PERIOD_MS` | int | `5` | Runtime lower clamp for stream period. |
| `CONFIG_IMU_BMO_055_MAX_STREAM_PERIOD_MS` | int | `1000` | Runtime upper clamp for stream period. |
| `CONFIG_IMU_BMO_055_STREAM_TASK_STACK` | int | `4096` | Stream task stack size. |
| `CONFIG_IMU_BMO_055_STREAM_TASK_PRIO` | int | `6` | Stream task priority. |
| `CONFIG_IMU_BMO_055_CONFIG_TASK_STACK` | int | `4096` | Configure task stack size. |
| `CONFIG_IMU_BMO_055_CONFIG_TASK_PRIO` | int | `6` | Configure task priority. |
| `CONFIG_IMU_BMO_055_CONFIG_QUEUE_DEPTH` | int | `8` | Configure command queue depth. |

## Public API
Public headers:
- `include/imu_bmo_055.hpp`
- `include/imu_bmo_055_types.hpp`
- `include/imu_bmo_055_protocol.hpp`

### Main class
- `ImuBno055Manager(uart_comm::UartComm&, const ManagerConfig&)`
- `bool isValid() const`
- Runtime controls: `setStreamEnabled`, `setStreamPeriodMs`, `setStreamFieldMask`, `setOperationMode`
- Calibration: `readCalibration`, `writeCalibration`, `applyCalibration`
- RX entrypoint: `handleConfigureCommand`

### Protocol/typing
- `ResultCode`, `ConfigureSubcommand`, `StreamField`
- `ManagerConfig`, `I2cConfig`, `StreamTaskConfig`
- `CalibrationBlob`, `CalibrationStatus`

## Usage Example
```cpp
#include "uart_comm.hpp"
#include "imu_bmo_055.hpp"

extern "C" void app_main(void)
{
    uart_comm::UartComm uart({});
    if (!uart.isValid()) {
        return;
    }

    imu_bmo_055::ImuBno055Manager imu(uart, {});
    if (!imu.isValid()) {
        return;
    }

    imu.setStreamEnabled(true);
    imu.setStreamPeriodMs(20);
}
```

## Error Handling
`ResultCode` values:
- `Ok`, `InvalidArgument`, `InvalidState`, `NotSupported`, `Busy`, `InternalError`

Device-layer failures are mapped from `esp_err_t` and logged with context.

## Thread Safety
- Shared manager state is mutex-protected.
- Stop/valid flags are atomic.
- Callback only enqueues work; heavy parsing executes in config task.

## Limitations & Known Issues
- Only one active BNO055 device instance is supported due static callback bridge.
- Event message type is reserved; event payload emission is not implemented.
- Stream payload is bounded by `CONFIG_UART_COMM_MAX_PAYLOAD`.
