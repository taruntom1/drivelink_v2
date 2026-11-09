# IMU BNO055 Protocol API Guide

This guide defines only the IMU component protocol contract.
Assumption: host already has a UART transport layer that can send and receive framed payloads.

## 1. Transport Assumptions

The IMU component uses the `uart_comm` type/channel model:

- Configure command message type: default `0x22`
- Configure response message type: default `0x42`
- Telemetry stream channel: default `0x82`

These IDs are configurable in ESP Kconfig, so host and firmware must use the same values.

The IMU has no event message type of its own. Errors and stream health counters are reported by the `diag` component (event `0xC0`, source `imu`; see `components/diag/README.md`).

All multi-byte integers in IMU payloads are little-endian.

## 2. Common Conventions

### 2.1 Protocol version

- Current version: `0x03`
- All configure command and response payloads start with protocol version byte.
- All stream payloads start with protocol version byte.
- Commands with any other version are rejected (see section 4).

### 2.2 Changes from v2

- Subcommand `0x02` is now `SetStreamTiming`: `u32 sample_period_us | u32 telemetry_period_us`, and its response carries the applied values. It replaces `SetStreamPeriodMs` (`u16 period_ms`).
- Every configure body must have its **exact** length. v2 accepted trailing bytes; v3 answers `InvalidArgument` and applies nothing.
- Stricter values: `SetStreamEnabled` takes only `0` or `1`, `SetStreamFieldMask` rejects bits outside `0x01FF`, and the calibration blob's `has_sic` must be `0` or `1`.
- The stream `timestamp_us` is now wall-clock time (the time_sync-corrected clock, the same one motor telemetry uses), taken at the midpoint of the sample's I2C reads. In v2 it was `esp_timer` time since boot, taken before the reads.
- The stream layout is otherwise unchanged; it carries version `0x03`.
- Device-busy failures (sensor held by another operation) now return `Busy` instead of `InternalError`.

### 2.3 Result codes (response byte 3)

- `0x00` = `Ok`
- `0x01` = `InvalidArgument`
- `0x02` = `InvalidState`
- `0x03` = `NotSupported`
- `0x04` = `Busy`
- `0x05` = `InternalError`

### 2.4 Configure subcommands

| ID | Name | Body | Body length |
|---|---|---|---|
| `0x01` | `SetStreamEnabled` | `u8 enabled` (0 or 1) | 1 |
| `0x02` | `SetStreamTiming` | `u32 sample_period_us`, `u32 telemetry_period_us` | 8 |
| `0x03` | `SetStreamFieldMask` | `u16 field_mask` (bits ⊆ `0x01FF`) | 2 |
| `0x04` | `SetOperationMode` | `u8 mode` (`0x00..0x0C`) | 1 |
| `0x05` | `ReadCalibration` | none | 0 |
| `0x06` | `WriteCalibration` | 41-byte calibration blob | 41 |
| `0x07` | `ApplyCalibration` | none | 0 |

## 3. Configure Command Payloads (Host -> ESP)

General shape:

- Byte 0: `version` (`0x03`)
- Byte 1: `seq`, chosen by the host and echoed in the response
- Byte 2: `subcommand`
- Byte 3..N: subcommand body, exactly the length in the table above

Commands are queued and answered in order. Give every in-flight command a different `seq` (for example a wrapping counter) and match each response on `seq`.

### 3.1 SetStreamEnabled (`subcommand = 0x01`)

- Byte 3: `enabled` (`0x00` disable, `0x01` enable; any other value is rejected)

Total length: 4

Disabling stops the sampling timer. Enabling starts it; the first sample after enabling is always published.

### 3.2 SetStreamTiming (`subcommand = 0x02`)

- Byte 3..6: `sample_period_us` (`uint32` LE): how often the sensor is read
- Byte 7..10: `telemetry_period_us` (`uint32` LE): how often a stream packet is sent

Total length: 11

Rules:

- A value of `0` keeps the current setting, so `SetStreamTiming(0, 0)` reads the current timing without changing it.
- `sample_period_us` is clamped to the firmware's `[min, max]` (Kconfig defaults `5000..1000000`).
- `telemetry_period_us` is clamped to `[sample_period, max]` and rounded **up** to a whole multiple of the sample period: a packet is sent every N samples.
- The firmware stores the **requested** telemetry period. When the sample period changes later, N is recomputed, so the telemetry rate stays as close as possible to what the host asked for.
- The response body (on `Ok`) reports the values actually applied (section 4).

In the BNO055 fusion modes the sensor updates its outputs at 100 Hz, so a sample period below `10000` only gives fresh data in the raw (non-fusion) modes.

### 3.3 SetStreamFieldMask (`subcommand = 0x03`)

- Byte 3..4: `field_mask` (`uint16` LE). Bits outside `0x01FF` are rejected.

Total length: 5

### 3.4 SetOperationMode (`subcommand = 0x04`)

- Byte 3: operation mode (`uint8`), `0x00..0x0C`

Total length: 4

The stream is paused during the mode switch (20–600 ms) and resumed if it was enabled.

### 3.5 ReadCalibration (`subcommand = 0x05`)

No body. Total length: 3

### 3.6 WriteCalibration (`subcommand = 0x06`)

- Byte 3..43: calibration blob (41 bytes)

Total length: 44

Blob layout (starts at command Byte 3):

- Byte 0: `has_sic` (`0` or `1`; any other value is rejected)
- Byte 1..2: accel_offset_x (s16)
- Byte 3..4: accel_offset_y (s16)
- Byte 5..6: accel_offset_z (s16)
- Byte 7..8: accel_radius (s16)
- Byte 9..10: gyro_offset_x (s16)
- Byte 11..12: gyro_offset_y (s16)
- Byte 13..14: gyro_offset_z (s16)
- Byte 15..16: mag_offset_x (s16)
- Byte 17..18: mag_offset_y (s16)
- Byte 19..20: mag_offset_z (s16)
- Byte 21..22: mag_radius (s16)
- Byte 23..24: sic[0] (s16)
- Byte 25..26: sic[1] (s16)
- Byte 27..28: sic[2] (s16)
- Byte 29..30: sic[3] (s16)
- Byte 31..32: sic[4] (s16)
- Byte 33..34: sic[5] (s16)
- Byte 35..36: sic[6] (s16)
- Byte 37..38: sic[7] (s16)
- Byte 39..40: sic[8] (s16)

### 3.7 ApplyCalibration (`subcommand = 0x07`)

No body. Total length: 3

## 4. Configure Response Payloads (ESP -> Host)

General shape:

- Byte 0: version (`0x03`)
- Byte 1: `seq` echo
- Byte 2: subcommand echo
- Byte 3: result code
- Byte 4..N: optional response data

Response data, sent only when the result is `Ok`:

- `SetStreamTiming`: bytes 4..7 `applied_sample_period_us` (`uint32` LE), bytes 8..11 `applied_telemetry_period_us` (`uint32` LE). The telemetry value is always a whole multiple of the sample value.
- `ReadCalibration`: bytes 4..44 carry the 41-byte calibration blob in the same format as WriteCalibration.
- Every other subcommand: no data (response length 4).

If the command header could not be parsed (payload shorter than 3 bytes, or wrong `version`), the response has subcommand `0xFF` and result `InvalidArgument`. `seq` is echoed when the command's version was right and it carried a `seq` byte; otherwise it is `0`.

## 5. Telemetry Stream Payload (`stream channel`, default `0x82`)

### 5.1 Base header (always present)

- Byte 0: version (`0x03`)
- Byte 1..2: field_mask (`uint16` LE)
- Byte 3..10: timestamp_us (`uint64` LE)

`timestamp_us` is wall-clock time in µs: the clock time_sync corrects (`gettimeofday` on the ESP), the same one motor telemetry uses, so IMU and motor samples line up directly. It is taken at the midpoint of that sample's I2C reads. It can jump when time_sync steps the clock.

Packets are sent every N samples (see `SetStreamTiming`), so consecutive timestamps are about `applied_telemetry_period_us` apart, plus jitter.

### 5.2 Field mask bits

- Bit 0 (`0x0001`): Euler
- Bit 1 (`0x0002`): Quaternion
- Bit 2 (`0x0004`): LinearAccel
- Bit 3 (`0x0008`): Gravity
- Bit 4 (`0x0010`): RawAccel
- Bit 5 (`0x0020`): RawGyro
- Bit 6 (`0x0040`): RawMag
- Bit 7 (`0x0080`): Temperature
- Bit 8 (`0x0100`): CalibrationStatus

Default mask is `0x01FF` (all fields).

### 5.3 Field append order and sizes

When a bit is set, data is appended in this exact order:

1. Euler: `h, r, p` as `s16` each (6 bytes)
2. Quaternion: `w, x, y, z` as `s16` each (8 bytes)
3. LinearAccel: `x, y, z` as `s16` each (6 bytes)
4. Gravity: `x, y, z` as `s16` each (6 bytes)
5. RawAccel: `x, y, z` as `s16` each (6 bytes)
6. RawGyro: `x, y, z` as `s16` each (6 bytes)
7. RawMag: `x, y, z` as `s16` each (6 bytes)
8. Temperature: `int8` (1 byte)
9. CalibrationStatus: `sys, gyro, accel, mag` as `uint8` each (4 bytes)

Total length depends on active mask.

### 5.4 Max payload

Encoded stream payload is bounded by `CONFIG_UART_COMM_MAX_PAYLOAD`.
If a chosen mask could exceed transport max, firmware drops that packet and counts it (`encode_errors`).

## 6. Host Implementation Checklist

1. Use matching message IDs/channels from firmware config.
2. Always set version byte to `0x03` in configure commands, followed by a `seq` byte.
3. Send each body at its exact length (section 2.4).
4. Parse response as `[version, seq, subcommand, result, optional...]` and match it to its command by `seq`.
5. Parse stream header first, then decode fields strictly by bitmask and fixed append order.
6. Use little-endian decode for all multi-byte values.
7. Treat `timestamp_us` as wall-clock µs on the same time base as motor telemetry.

## 7. Recommended Host Command Sequence

Initial bring-up sequence:

1. `SetStreamEnabled(0)`
2. `SetOperationMode(0x0C)` (or desired mode)
3. `SetStreamFieldMask(...)`
4. `SetStreamTiming(sample_us, telemetry_us)`, and keep the applied values from the response
5. `SetStreamEnabled(1)`

Calibration restore sequence:

1. `SetStreamEnabled(0)`
2. `WriteCalibration(blob)`
3. `ApplyCalibration()`
4. `SetStreamEnabled(1)`

## 8. Error Handling Guidance

- `InvalidArgument` usually means wrong body length, version mismatch, or an invalid value (mode, mask bits, `enabled`, `has_sic`). A subcommand echo of `0xFF` means the command header itself was rejected.
- `InvalidState` usually means manager/device not ready, or calibration apply requested before write/read cache.
- `Busy` means mutex or resource contention, including the sensor being held by another operation (a mode switch or calibration in progress). Retrying later is fine.
- `InternalError` means a sensor operation failed at device layer (I2C error), or the sampling timer could not be started or retimed.

Host should log the subcommand + result and retry only where appropriate.
