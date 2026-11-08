# IMU BNO055 Protocol API Guide

This guide defines only the IMU component protocol contract.
Assumption: host already has a UART transport layer that can send and receive framed payloads.

## 1. Transport Assumptions

The IMU component uses the `uart_comm` type/channel model:

- Configure command message type: default `0x22`
- Configure response message type: default `0x42`
- Event message type: default `0xC2` (reserved; no event payload currently emitted)
- Telemetry stream channel: default `0x82`

These IDs are configurable in ESP Kconfig, so host and firmware must use the same values.

All multi-byte integers in IMU payloads are little-endian.

## 2. Common Conventions

### 2.1 Protocol version

- Current version: `0x02`
- All configure command and response payloads start with protocol version byte.
- All stream payloads start with protocol version byte.
- Version 2 added a `seq` byte after `version` in configure commands and responses. The stream layout did not change, but stream payloads carry version `0x02` as well. Version 1 commands are rejected.

### 2.2 Result codes (response byte 3)

- `0x00` = `Ok`
- `0x01` = `InvalidArgument`
- `0x02` = `InvalidState`
- `0x03` = `NotSupported`
- `0x04` = `Busy`
- `0x05` = `InternalError`

### 2.3 Configure subcommands

- `0x01` = `SetStreamEnabled`
- `0x02` = `SetStreamPeriodMs`
- `0x03` = `SetStreamFieldMask`
- `0x04` = `SetOperationMode`
- `0x05` = `ReadCalibration`
- `0x06` = `WriteCalibration`
- `0x07` = `ApplyCalibration`

## 3. Configure Command Payloads (Host -> ESP)

General shape:

- Byte 0: `version`
- Byte 1: `seq`, chosen by the host and echoed in the response
- Byte 2: `subcommand`
- Byte 3..N: subcommand payload

Commands are queued and answered in order. Give every in-flight command a different `seq` (for example a wrapping counter) and match each response on `seq`.

### 3.1 SetStreamEnabled (`subcommand = 0x01`)

Payload bytes:

- Byte 0: `0x02` version
- Byte 1: `seq`
- Byte 2: `0x01` subcommand
- Byte 3: `enabled` (`0x00` disable, non-zero enable)

Total length: 4

### 3.2 SetStreamPeriodMs (`subcommand = 0x02`)

Payload bytes:

- Byte 0: version
- Byte 1: `seq`
- Byte 2: subcommand
- Byte 3..4: `period_ms` (`uint16` LE)

Total length: 5

Firmware clamps value to configured `[min_stream_period_ms, max_stream_period_ms]`.

### 3.3 SetStreamFieldMask (`subcommand = 0x03`)

Payload bytes:

- Byte 0: version
- Byte 1: `seq`
- Byte 2: subcommand
- Byte 3..4: `field_mask` (`uint16` LE)

Total length: 5

### 3.4 SetOperationMode (`subcommand = 0x04`)

Payload bytes:

- Byte 0: version
- Byte 1: `seq`
- Byte 2: subcommand
- Byte 3: operation mode (`uint8`)

Total length: 4

Supported range: `0x00..0x0C`.

### 3.5 ReadCalibration (`subcommand = 0x05`)

Payload bytes:

- Byte 0: version
- Byte 1: `seq`
- Byte 2: subcommand

Total length: 3

### 3.6 WriteCalibration (`subcommand = 0x06`)

Payload bytes:

- Byte 0: version
- Byte 1: `seq`
- Byte 2: subcommand
- Byte 3..43: calibration blob (41 bytes)

Total length: 44

Blob layout (starts at command Byte 3):

- Byte 0: `has_sic` (`0` or `1`)
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

Host must provide all 41 blob bytes.

### 3.7 ApplyCalibration (`subcommand = 0x07`)

Payload bytes:

- Byte 0: version
- Byte 1: `seq`
- Byte 2: subcommand

Total length: 3

## 4. Configure Response Payloads (ESP -> Host)

General shape:

- Byte 0: version
- Byte 1: `seq` echo
- Byte 2: subcommand echo
- Byte 3: result code
- Byte 4..N: optional response data

For all subcommands except `ReadCalibration`, optional data length is 0.

For `ReadCalibration`, if result is `Ok`, bytes 4..44 carry the 41-byte calibration blob in the same format as WriteCalibration.

If the command header could not be parsed (payload shorter than 3 bytes, or wrong `version`), the response has subcommand `0xFF` and result `InvalidArgument`. `seq` is echoed when the command's version was right and it carried a `seq` byte; otherwise it is `0`.

## 5. Telemetry Stream Payload (`stream channel`, default `0x82`)

### 5.1 Base header (always present)

- Byte 0: version (`0x02`)
- Byte 1..2: field_mask (`uint16` LE)
- Byte 3..10: timestamp_us (`uint64` LE)

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
If a chosen mask could exceed transport max, firmware drops that packet and logs overflow.

## 6. Host Implementation Checklist

1. Use matching message IDs/channels from firmware config.
2. Always set version byte to `0x02` in configure commands, followed by a `seq` byte.
3. Parse response as `[version, seq, subcommand, result, optional...]` and match it to its command by `seq`.
4. Parse stream header first, then decode fields strictly by bitmask and fixed append order.
5. Use little-endian decode for all multi-byte values.
6. Keep calibration blob exactly 41 bytes when sending WriteCalibration.

## 7. Recommended Host Command Sequence

Initial bring-up sequence:

1. `SetStreamEnabled(0)`
2. `SetOperationMode(0x0C)` (or desired mode)
3. `SetStreamFieldMask(...)`
4. `SetStreamPeriodMs(...)`
5. `SetStreamEnabled(1)`

Calibration restore sequence:

1. `SetStreamEnabled(0)`
2. `WriteCalibration(blob)`
3. `ApplyCalibration()`
4. `SetStreamEnabled(1)`

## 8. Error Handling Guidance

- `InvalidArgument` usually means wrong payload length, version mismatch, or invalid mode. A subcommand echo of `0xFF` means the command header itself was rejected.
- `InvalidState` usually means manager/device not ready, or calibration apply requested before write/read cache.
- `Busy` means mutex or resource contention.
- `InternalError` means sensor operation failed at device layer.

Host should log the subcommand + result and retry only where appropriate.
