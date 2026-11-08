# imu_bmo_055 Component

ESP-IDF IMU component for Bosch BNO055 with:

- I2C sensor bring-up and sampling
- Runtime configuration over `uart_comm` commands
- Periodic telemetry streaming over a dedicated stream channel
- Calibration read/write/apply command support

This component is designed to run on-device (ESP32-S3 in this project) and expose a stable wire protocol to a host application.

## 1. What The Component Does

At runtime, the component creates an `ImuBno055Manager` that:

1. Initializes the BNO055 on I2C.
2. Registers a command callback on a configurable UART message type.
3. Runs a config task for command processing.
4. Runs a stream task for periodic IMU telemetry.

The manager is non-copyable and exposes `isValid()` so startup can fail early and safely.

## 2. Public API (ESP Side)

Public header: `include/imu_bmo_055.hpp`

Primary class:

- `imu_bmo_055::ImuBno055Manager`

Primary config struct:

- `imu_bmo_055::ManagerConfig` (in `include/imu_bmo_055_types.hpp`)

Important runtime methods:

- `setStreamEnabled(bool)`
- `setStreamPeriodMs(uint16_t)`
- `setStreamFieldMask(uint16_t)`
- `setOperationMode(uint8_t)`
- `readCalibration(CalibrationBlob&)`
- `writeCalibration(const CalibrationBlob&)`
- `applyCalibration()`

## 3. Quick Integration In ESP App

1. Add component dependency where your app component is declared.
2. Construct `uart_comm::UartComm` first.
3. Construct `ImuBno055Manager` with UART instance and optional `ManagerConfig` overrides.
4. Check `isValid()` and fail startup if false.

Example:

```cpp
#include "uart_comm.hpp"
#include "imu_bmo_055.hpp"

extern "C" void app_main(void)
{
    uart_comm::UartComm uart(uart_comm::UartCommConfig{});
    if (!uart.isValid()) {
        return;
    }

    imu_bmo_055::ManagerConfig imu_cfg{}; // defaults from Kconfig
    imu_bmo_055::ImuBno055Manager imu(uart, imu_cfg);
    if (!imu.isValid()) {
        return;
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

## 3.1 Hardware-Free Mock Telemetry

If you do not have a BNO055 connected, enable mock telemetry either with Kconfig
or directly in code:

```cpp
imu_bmo_055::ManagerConfig imu_cfg{};
imu_cfg.use_mock_data = true;
imu_bmo_055::ImuBno055Manager imu(uart, imu_cfg);
```

Related Kconfig options:

- `IMU_BMO_055_USE_MOCK_DATA`
- `IMU_BMO_055_FALLBACK_TO_MOCK_ON_INIT_FAIL`

When mock mode is enabled, the manager keeps the same UART protocol and stream
channel but publishes synthesized BNO055-like samples.

## 4. Runtime Data And Behavior

The stream task publishes packets with this fixed prefix:

- `version` (1 byte)
- `field_mask` (uint16, little-endian)
- `timestamp_us` (uint64, little-endian, from `esp_timer_get_time()`)

Then field data is appended in fixed order based on mask bits:

1. Euler (h, r, p)
2. Quaternion (w, x, y, z)
3. Linear Accel (x, y, z)
4. Gravity (x, y, z)
5. Raw Accel (x, y, z)
6. Raw Gyro (x, y, z)
7. Raw Mag (x, y, z)
8. Temperature
9. Calibration Status (sys, gyro, accel, mag)

All vector/scalar numeric values above are encoded as signed 16-bit little-endian except temperature (1 byte) and calibration status (4 bytes total).

For complete host payload details, see:

- `docs/protocol-api-guide.md`

## 5. Kconfig Configuration

Menu path:

- `IMU BNO055 Configuration`

### 5.1 Protocol IDs

- `IMU_BMO_055_CONFIGURE_MSG_TYPE` (default `0x22`)
- `IMU_BMO_055_CONFIGURE_RESPONSE_MSG_TYPE` (default `0x42`)
- `IMU_BMO_055_EVENT_MSG_TYPE` (default `0xC2`)
- `IMU_BMO_055_STREAM_CHANNEL` (default `0x82`)

### 5.2 I2C Sensor Config

- `IMU_BMO_055_I2C_PORT_NUM`
- `IMU_BMO_055_I2C_SDA_PIN`
- `IMU_BMO_055_I2C_SCL_PIN`
- `IMU_BMO_055_I2C_CLK_HZ`
- `IMU_BMO_055_I2C_TIMEOUT_MS`
- `IMU_BMO_055_DEVICE_ADDRESS`

### 5.3 IMU Runtime Defaults

- `IMU_BMO_055_DEFAULT_OPERATION_MODE` (default `0x0C`, NDOF)
- `IMU_BMO_055_DEFAULT_STREAM_FIELD_MASK` (default `0x1FF`, all fields)
- `IMU_BMO_055_DEFAULT_STREAM_PERIOD_MS` (default `10` ms)
- `IMU_BMO_055_MIN_STREAM_PERIOD_MS`
- `IMU_BMO_055_MAX_STREAM_PERIOD_MS`

### 5.4 Tasking And Queue

- `IMU_BMO_055_STREAM_TASK_STACK`
- `IMU_BMO_055_STREAM_TASK_PRIO`
- `IMU_BMO_055_CONFIG_TASK_STACK`
- `IMU_BMO_055_CONFIG_TASK_PRIO`
- `IMU_BMO_055_CONFIG_QUEUE_DEPTH`

## 6. Calibration Support

Supported command flow:

1. Host requests current calibration (`ReadCalibration`).
2. Host stores calibration blob externally if desired.
3. Host writes blob back (`WriteCalibration`).
4. Host applies blob to sensor (`ApplyCalibration`).

Notes:

- `WriteCalibration` caches data in manager state.
- `ApplyCalibration` pushes cached data to BNO055 registers.
- Applying without cached data returns `InvalidState`.

## 7. Build And Validation

Typical workflow:

```bash
idf.py menuconfig
idf.py build
idf.py flash
idf.py monitor
```

If target/config state becomes inconsistent:

```bash
idf.py fullclean
idf.py build
```

## 8. Troubleshooting

- `isValid() == false` at startup:
  - Check I2C pins/port/address Kconfig values.
  - Check BNO055 wiring and power.
  - Check UART component validity before constructing IMU manager.
- No stream packets on host:
  - Verify stream channel ID matches host expectation.
  - Verify stream is enabled and period is not too large.
  - Confirm host UART layer is consuming stream range (`0x80..0xBF`).
- Command rejected:
  - Verify protocol version byte is `0x02` and is followed by a `seq` byte.
  - Verify payload length and little-endian encoding for numeric fields.

## 9. Additional Docs

- Protocol and host integration guide: `docs/protocol-api-guide.md`
- Internal architecture details: `docs/internal-architecture.md`
