# IMU BNO055 Internal Architecture

This document describes the internal architecture of the `imu_bmo_055` ESP-IDF component so engineers can safely modify or extend it.

## 1. Design Goals

- Keep public IMU API small and hardware-focused.
- Hide low-level BNO055/I2C details behind an internal device wrapper.
- Separate protocol handling from sensor I/O.
- Support high-rate stream traffic and command traffic concurrently.
- Keep shutdown deterministic and object ownership explicit.

## 2. File-Level Structure

- `include/imu_bmo_055.hpp`
  - Public manager API (`ImuBno055Manager`)
- `include/imu_bmo_055_types.hpp`
  - Public config structs, result codes, calibration data structs
- `include/imu_bmo_055_protocol.hpp`
  - Protocol constants, subcommand IDs, stream field IDs
- `priv_include/bno055_device.hpp`
  - Internal BNO055 device wrapper interface
- `src/bno055_device.cpp`
  - I2C + Bosch driver integration and raw sensor/calibration access
- `src/imu_bmo_055.cpp`
  - Manager lifecycle, command decode/dispatch, stream encode/send, tasks
- `Kconfig`
  - Protocol IDs, I2C defaults, runtime limits, task/queue sizing
- `CMakeLists.txt`
  - ESP-IDF component registration and dependencies

## 3. Core Runtime Objects

### 3.1 ImuBno055Manager

Owns:

- Reference to `uart_comm::UartComm`
- Internal mutex for shared state
- Config queue for command frames
- Config task handle
- Stream task handle and its sampling timer (`rtos_util::TaskTimer`)
- Stream state, written under the mutex and read lock-free by the stream task: `enabled`, `field_mask`, `sample_period_us`, `telemetry_divider` (atomics); `operation_mode` and the requested telemetry period (mutex only)
- Stream counters (`late`, `busy`, `read_errors`, `encode_errors`; atomics)
- Cached calibration blob
- Internal `Bno055Device` instance

Public mutating APIs call into the same state model used by command handlers.

### 3.2 Bno055Device

Wraps Bosch C driver (`bno055.c`) and provides:

- I2C bus configuration and driver lifecycle
- BNO055 init and chip-id check
- Operation mode set/get
- Masked sample read for the stream tick (`tryReadSample`): try-locks the device, stamps the wall-clock midpoint of the I2C burst
- Bounded (100 ms) device lock for config operations; rate-limited bus error logs
- Calibration read/write operations

It is currently singleton-like via static `s_active_device` to satisfy Bosch callback signatures.
Only one active instance is supported.

## 4. Lifecycle And Initialization Flow

1. `ImuBno055Manager` constructor validates UART and allocates mutex.
2. Runtime defaults are initialized from `ManagerConfig`: the period bounds are validated (min > 0, min ≤ max), then the sample period is clamped and unknown field-mask bits are dropped.
3. `Bno055Device` is created and validated.
4. Default operation mode is applied.
5. Config queue is created.
6. UART callback for configure message type is registered.
7. Config task is created.
8. Stream task is created if enabled, then its sampling timer is created and started.
9. Manager sets `m_valid = true`.

If any step fails, constructor exits early and object remains invalid.

## 5. Concurrency Model

### 5.1 Tasks

- Config task (`imu_cfg`)
  - Consumes queued configure frames
  - Decodes and dispatches subcommands
  - Sends command responses

- Stream task (`imu_stream`)
  - Woken by the sampling timer (`ulTaskNotifyTake`, bounded at 2 periods + 100 ms)
  - Reads one sample per wake via `Bno055Device::tryReadSample` (never waits for the device)
  - Every Nth sample: encodes the payload for the current field mask and sends it via `uart_comm::sendData`

### 5.2 RX callback behavior

Registered callback only enqueues command frame to config queue.
No heavy decode or sensor operations are done in RX task context.

### 5.3 Shared-state synchronization

The manager mutex serializes writers (config task, public API) of the stream settings, the timer (start/stop/retime) and the calibration cache.
The stream task never takes it: it reads the stream settings from atomics.
Atomic flags are used for stop signals and validity state.

## 6. Command Processing Pipeline

1. Host sends configure packet on configured command message type.
2. UART callback enqueues `RxFrame` to `m_config_queue`.
3. Config task dequeues frame and checks the header (`version`, `seq`, `subcommand`).
4. `parseAndDispatchConfigure()` gets the subcommand and the body after the header, and validates the body length.
5. Subcommand method runs and returns `ResultCode`.
6. `sendConfigureResponse()` emits standardized response payload.

### Important behavior details

- Every command gets a response.
- Unknown subcommand returns `NotSupported`.
- Invalid lengths/version return `InvalidArgument`. When the header itself is rejected, the response echoes subcommand `0xFF`.
- Every response echoes the command's `seq`.
- Every body must have its exact length (protocol v3).
- `SetStreamTiming` clamps the periods and echoes the applied values; 0 keeps a value.
- Mode set stops the stream and its timer during the mode switch and restores the prior enabled state.

## 7. Stream Processing Pipeline

1. The timer notifies the stream task. The notification count is the number of periods since the last wake; extra periods count as `late`.
2. If the stream is enabled, the task reads the field mask and calls `Bno055Device::tryReadSample(field_mask, sample)`. A busy device counts `busy`, an I2C failure `read_errors`.
3. The telemetry countdown decides whether this sample is published (every N samples; a publish slot whose read failed stays pending).
4. `encodeStreamSample()` builds the payload with the fixed header and mask-ordered fields, and it is sent on the configured stream channel.
5. At most once per second, if any counter changed, a summary is logged.

Encoding is bounded by `CONFIG_UART_COMM_MAX_PAYLOAD`.
If overflow is detected, the packet is dropped and `encode_errors` is incremented.

## 8. Calibration Data Path

- `ReadCalibration`:
  - Reads offsets/radii (and optional SIC) from sensor
  - Stores result in manager cache
  - Returns 41-byte blob in response payload

- `WriteCalibration`:
  - Validates/decode 41-byte blob
  - Updates manager cache only

- `ApplyCalibration`:
  - Writes cached blob to sensor registers
  - Temporarily uses BNO055 config mode if needed
  - Restores previous mode

This split allows host to stage and apply calibration deterministically.

## 9. Error Handling Strategy

- Manager-level operations map failures to `ResultCode` for host responses.
- Device-level operations return `esp_err_t` and log concrete details.
- Most APIs fail fast if object is invalid.
- Queue full on command enqueue logs warning and drops frame.

## 10. Resource Management

- Mutexes/queues/tasks are owned by manager and cleaned in destructor.
- Bno055Device owns and deletes I2C driver if it installed it.
- Stop sequence: request stop on both tasks, wake the config queue, join the config task (the only other timer user), destroy the timer (waiting out an in-flight callback), then join the stream task.

## 11. Extension Guidance

### Adding a new configure command

1. Add enum in `ConfigureSubcommand`.
2. Implement parse branch in `parseAndDispatchConfigure()`.
3. Add any payload encode/decode helper if needed.
4. Return standardized `ResultCode` and response.
5. Update protocol docs and host parser.

### Adding a new stream field

1. Add bit in `StreamField` enum.
2. Extend `ImuStreamSample` struct.
3. Read sensor value in `Bno055Device::tryReadSample()` gated by bit.
4. Append encoded bytes in `encodeStreamSample()` in deterministic order.
5. Update protocol docs with new mask bit, order, and size.

### Supporting multiple BNO055 devices

Current static callback bridge enforces single active device.
To support multiple instances, redesign callback context mapping away from single static pointer.

## 12. Known Constraints

- Single BNO055 active instance per process.
- Event message ID is reserved but not currently used for emitted event payloads.
- Stream payload is limited by UART comm max payload config.
- Units/scales are those produced by BNO055 register reads under current sensor unit config.
