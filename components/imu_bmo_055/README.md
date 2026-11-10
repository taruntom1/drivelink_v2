# imu_bmo_055 Component

ESP-IDF IMU component for Bosch BNO055 with:

- I2C sensor bring-up and sampling
- Runtime configuration over `uart_comm` commands
- Timer-driven sampling (esp_timer, independent of `CONFIG_FREERTOS_HZ`), with sample and telemetry rates the host can change at runtime
- Telemetry streaming over a dedicated stream channel, stamped with the time_sync-corrected wall clock
- Calibration read/write/apply command support

This component is designed to run on-device (ESP32-S3 in this project) and expose a stable wire protocol to a host application.

## 1. What The Component Does

At runtime, the component creates an `ImuBno055Manager` that:

1. Initializes the BNO055 on I2C.
2. Registers a command callback on a configurable UART message type.
3. Runs a config task for command processing.
4. Runs a stream task, woken by a periodic `esp_timer`, that samples the sensor and publishes telemetry (see section 4.1).

The manager is non-copyable and exposes `isValid()` so startup can fail early and safely.

## 2. Public API (ESP Side)

Public header: `include/imu_bmo_055.hpp`

Primary class:

- `imu_bmo_055::ImuBno055Manager`

Primary config struct:

- `imu_bmo_055::ManagerConfig` (in `include/imu_bmo_055_types.hpp`)

Wire-level types (`ResultCode`, `CalibrationStatus`, `CalibrationBlob`, subcommands, stream fields and sizes) are in `include/imu_bmo_055_protocol.hpp`, which `imu_bmo_055_types.hpp` includes. The payload codec (`priv_include/imu_protocol.hpp`, `src/imu_protocol.cpp`) has no ESP-IDF dependency and is covered by the host unit tests in `test/host/`.

Important runtime methods:

- `setStreamEnabled(bool)`: also starts or stops the sampling timer
- `setStreamTiming(sample_period_us, telemetry_period_us, applied_sample_us&, applied_telemetry_us&)`: 0 keeps a value; returns the applied (clamped, rounded) values
- `setStreamFieldMask(uint16_t)`: rejects bits outside `kStreamFieldAll` (`0x01FF`)
- `setOperationMode(uint8_t)`
- `readCalibration(CalibrationBlob&)`
- `writeCalibration(const CalibrationBlob&)`
- `applyCalibration()`
- Stream health is counted through diag (see 4.1); there is no on-device getter.

Device-level failures map to `Busy` when the sensor is held by another operation (lock wait timed out, 100 ms) and to `InternalError` otherwise.

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
- `timestamp_us` (uint64, little-endian): wall-clock µs (`gettimeofday`, the clock time_sync corrects, the same one motor telemetry uses), taken at the midpoint of that sample's I2C reads. It can jump when time_sync steps the clock.

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

## 4.1 Sampling And Telemetry Timing

- A periodic `esp_timer` (`rtos_util::TaskTimer`, task dispatch) wakes the stream task every `sample_period_us`. The loop does not use the FreeRTOS tick, so its rate does not depend on `CONFIG_FREERTOS_HZ`.
- Every wake reads one sample. A stream packet goes out every N samples, where N is the requested `telemetry_period_us` clamped to `[sample period, max]` and rounded up to a whole multiple of the sample period. N = 1 by default.
- Samples read between packets are dropped today. That is the hook for a future on-board consumer when sampling faster than the telemetry rate.
- In the fusion modes the BNO055 updates its outputs at 100 Hz, so sampling faster than every 10 ms only gives fresh data in the raw (non-fusion) modes.
- The tick never waits:
  - It reads the stream settings from atomics, without the manager mutex.
  - It try-locks the device. If a config operation (mode switch, calibration) holds it, the tick is skipped and counted as `busy`.
- Counters:
  - `late`: timer periods that passed without a sample, because the task woke late or an I2C read ran long
  - `busy`: ticks skipped because the device was in use
  - `read_errors`: I2C failures
  - `encode_errors`: payload overflows
  - They are diag counters (`imu.SampleLate`, `imu.DeviceBusy`, `imu.ReadError`, `imu.EncodeError`), sent to the host as 1 Hz deltas. Nothing is logged per tick.
  - If the stream is enabled but the timer stops waking the task, `imu.SamplerStalled` is reported once and cleared when it fires again, the stream is disabled or the task stops. A wait that spans a timer restart (enable, `SetStreamTiming`) is not counted as a stall.
- When a publish tick fails (busy or read error), the next good sample is published, so the telemetry rate holds.
- Disabling the stream stops the timer. `SetOperationMode` stops it for the duration of the mode switch and restarts it if the stream was enabled. The first sample after re-enabling is always published.

## 5. Kconfig Configuration

Menu path:

- `IMU BNO055 Configuration`

### 5.1 Protocol IDs

- `IMU_BMO_055_CONFIGURE_MSG_TYPE` (default `0x22`)
- `IMU_BMO_055_CONFIGURE_RESPONSE_MSG_TYPE` (default `0x42`)
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
- `IMU_BMO_055_DEFAULT_SAMPLE_PERIOD_US` (default `10000`)
- `IMU_BMO_055_DEFAULT_TELEMETRY_PERIOD_US` (default `0` = same as the sample period)
- `IMU_BMO_055_MIN_SAMPLE_PERIOD_US` (default `5000`)
- `IMU_BMO_055_MAX_SAMPLE_PERIOD_US` (default `1000000`; also the telemetry period bound)

The constructor fails (`isValid() == false`) if min is 0 or greater than max.

### 5.4 Tasking And Queue

- `IMU_BMO_055_STREAM_TASK_STACK`
- `IMU_BMO_055_STREAM_TASK_PRIO`
- `IMU_BMO_055_CONFIG_TASK_STACK`
- `IMU_BMO_055_CONFIG_TASK_PRIO`
- `IMU_BMO_055_CONFIG_QUEUE_DEPTH`
- `IMU_BMO_055_MUTEX_TIMEOUT_MS` (default `50`): configure commands wait this long for the manager mutex, then fail with `BUSY`
- `IMU_BMO_055_DEVICE_LOCK_TIMEOUT_MS` (default `100`): mode switches and calibration read/write wait this long for the BNO055 device lock. The stream tick never waits.

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

## 4.2 Errors reported to the host

Besides the configure `result`, the manager reports through the `diag` component (events on `0xC0`, see `components/diag/README.md` for the full code table). Source `imu`:

| Code | Kind | When |
|---|---|---|
| `InitFailed` | error | The constructor failed. `arg0` is the stage (mutex, config, BNO055 device, default mode, queue, callback, tasks, timer). |
| `FallbackToMock` | warning | The BNO055 did not come up and `fallback_to_mock_on_init_failure` switched to mock data. `esp_err` is the device error. |
| `ModeSwitchFailed`, `CalibrationReadFailed`, `CalibrationApplyFailed` | error | A device operation failed on the bus. A busy device is only answered with `BUSY`. |
| `StreamReenableFailed` | error | The stream stayed off after a mode switch. |
| `TimerRestartFailed` | error | `SetStreamTiming` could not restart the sampling timer. |
| `SamplerStalled` | warning, clears | See 4.1. |
| `SampleLate`, `DeviceBusy`, `ReadError`, `EncodeError`, `ConfigQueueFull`, `ResponseTxDropped` | counters | Sent as 1 Hz deltas when they change. A command that arrives while the config queue is full is dropped without a response. |

## 8. Troubleshooting

- `isValid() == false` at startup:
  - The host gets `imu.InitFailed` (its `arg0` names the stage) and `system.ManagerStartFailed`.
  - Check I2C pins/port/address Kconfig values.
  - Check BNO055 wiring and power.
  - Check UART component validity before constructing IMU manager.
- No stream packets on host:
  - Verify stream channel ID matches host expectation.
  - Verify stream is enabled and the periods are not too large (`SetStreamTiming(0, 0)` reads them back).
  - Watch the diag counters `imu.DeviceBusy` and `imu.ReadError`, and the `imu.SamplerStalled` record.
  - Confirm host UART layer is consuming stream range (`0x80..0xBF`).
- Command rejected:
  - Verify protocol version byte is `0x03` and is followed by a `seq` byte.
  - Verify the body has its exact length (v3 rejects trailing bytes) and little-endian encoding for numeric fields.

## 9. Additional Docs

- Protocol and host integration guide: `docs/protocol-api-guide.md`
- Internal architecture details: `docs/internal-architecture.md`
