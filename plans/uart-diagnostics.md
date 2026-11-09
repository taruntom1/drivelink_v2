# Plan: error codes over the control UART (`diag` component)

Today, errors are mostly text logs on USB-Serial-JTAG. To see them, the host has to monitor a second port. This plan moves error reporting onto the control UART (UART0) as **numeric codes**. The text logs stay for bench work, and they can be removed entirely at compile time.

It covers these review items:
- `review/06-input-validation-and-error-handling.md` E5: `app_main` fails silently. This plan keeps the board up in a degraded state and reports the error.
- `review/06-input-validation-and-error-handling.md` E6: error codes are lossy. The detail byte (phase 3) and per-HAL failure codes fix part of it.

## What the survey found

There are about 190 log calls (100 `E`, 52 `W`, 39 `I`). They fall into five groups:

| Group | Examples | Reaches the host today? |
|---|---|---|
| Startup failures | MCPWM/PCNT pools, I²C/BNO055 init, mutex/queue/task creation, callback registration | No. `main.cpp` returns without saying anything. |
| Rejected host commands | bad GPIO, non-finite float, config queue full, bad header | Only as a coarse `ResultCode`. A full queue sends no response at all. |
| Runtime faults | control overruns, stalled timers, IMU late/busy/read/encode errors, setpoint rejects, sync failures | No. They are counted and logged at most once a second, but nothing is sent. |
| Link errors | CRC, overflow, framing/parity, discarded bytes | No. `UartComm::getErrorCounters()` exists, but nothing calls it. |
| Programmer errors | `sendCommand` misuse, `stop()` called from inside its own task, registrations outliving `UartComm` | Not useful to the host. These stay as logs and asserts. |

## Decisions

| Topic | Decision |
|---|---|
| Approach | A new central component, `components/diag`. One event MSG_TYPE and one record format serve every source. |
| Delivery | Push and pull. The ESP pushes events that carry a `u16 seq`, and the host can query the latched state. There is no ACK and no retransmission. On a seq gap or a reconnect, the host queries. |
| Severities on the wire | Error and Warning. Info stays console-only. |
| Text logs | `DIAG_*` macros write the console line and the code in one call. Kconfig `DIAG_CONSOLE_LOG` removes the text part at compile time (format strings, args and calls). The IDF log level (`CONFIG_LOG_DEFAULT_LEVEL_NONE`) removes the rest. |
| UART reporting | Always on. No Kconfig switch. |
| Who reports | The managers, plus `main` for startup. `motor_hal`, `rotary_encoder_hal`, `rtos_util` and `pid` keep returning `esp_err_t` / `bool` and do not depend on `diag`. The reporter reads link errors through `getErrorCounters()`, so `uart_comm` does not depend on `diag` either. |
| Record context | Fixed size: `esp_err` (i32) plus two `u32` args. Each code documents what its args mean. |
| Frequent faults | Atomic counters in a fixed table. A delta event goes out at most once a second, and only when something changed. Totals can be queried. |
| Startup failure | Stay up in a degraded state. `UartComm` and `diag` keep running, a failed manager is left out, and the others keep running. The fault is latched and reported. `app_main` never returns. |
| Code table | A single `diag_codes.yaml`. A script generates the C++ header, the host Python table and the README table. CI fails if the generated files are stale. |
| Command rejections | A `detail` byte in the motor and IMU configure responses (both go to **v4**). Diag events are only for asynchronous faults. |
| Timestamp | Wall-clock µs (`gettimeofday`, the same clock as the streams), plus a `time_synced` flag bit. |
| IDs | Event `0xC0`, query `0x23`, response `0x63`, all from Kconfig. The unused IMU event Kconfig (`0xC2`) is removed. |
| Query command | GetInfo (reset reason, firmware version, started managers, drop counts), GetEntries (latched faults and counter totals, paged), and Clear. |
| Latching | Each code has a sticky `occurred` bit, which only the host's Clear resets, and an `active` bit, which the firmware clears on recovery and announces with a `Cleared` event. |
| Rollout | Three PRs, listed below. Each one builds cleanly, and CI passes. |

### Decisions made while writing this plan (review these)

These were not asked about explicitly:
- **Global sink, no reference passing.** The code table and record queue live in static storage, reached through free functions and macros. Errors can then be recorded from any manager before `UartComm` exists, without threading a `diag::Sink&` through every config. The *reporter*, which owns the task, the UART registration and the sending, is an ordinary object built in `app_main` like the managers.
- **A `Boot` event.** It is pushed once when the reporter starts, carrying the reset reason. The host then learns about a reboot in the middle of a session (brownout, watchdog, panic) without having to poll.
- **One table for counters and latched faults.** Every code has one slot `{count, occurred, active}`. "Latched faults" and "counter totals" are then the same query (GetEntries), filtered to slots with `count > 0` or `active`.

## Wire format (`diag` protocol v1)

Little-endian, built with `uart_comm::wire::Reader` / `Writer`. All bodies have an exact length. The codec is pure code (`diag_protocol.cpp`) and is covered by host tests.

### Event: MSG_TYPE `0xC0` (`CONFIG_DIAG_EVENT_MSG_TYPE`)

Common header (5 bytes): `version u8 | seq u16 | kind u8 | flags u8`
- `seq` increments with every event of any kind and wraps at 0xFFFF. A gap means events were lost.
- `flags` bit0 is `time_synced`. The other bits are reserved and sent as 0.

| kind | Name | Body after the header | Length |
|---|---|---|---|
| 0x01 | Record | `timestamp_us u64 \| severity u8 \| source u8 \| code u16 \| esp_err i32 \| arg0 u32 \| arg1 u32` | 24 |
| 0x02 | CounterDelta | `timestamp_us u64 \| n u8 \| n × (source u8 \| code u16 \| delta u32 \| total u32)` | 9 + 11n |
| 0x03 | Cleared | `timestamp_us u64 \| source u8 \| code u16` | 11 |
| 0x04 | Boot | `timestamp_us u64 \| reset_reason u8 \| started_mask u8` | 10 |

- `severity` is 1 = Error, 2 = Warning.
- `reset_reason` is the `esp_reset_reason_t` value.
- `started_mask` has bit0 motor_manager, bit1 time_sync, bit2 imu.
- When several counters change in the same second, CounterDelta packs as many as fit in `CONFIG_UART_COMM_MAX_PAYLOAD` and sends more frames for the rest.

### Query: command `0x23` / response `0x63`

Request header: `version u8 | seq u8 | subcommand u8`. Response header: `version u8 | seq u8 | subcommand u8 | result u8`. A request whose header can't be parsed gets a response with subcommand `0xFF` (`kUnparsedHeaderByte`).

| Sub | Name | Request body | Response body (after the 4-byte header) |
|---|---|---|---|
| 0x01 | GetInfo | none | `flags u8 \| uptime_us u64 \| reset_reason u8 \| started_mask u8 \| records_dropped u32 \| events_tx_dropped u32 \| version_len u8 \| version[version_len]` (from `esp_app_get_description()`, at most 32 bytes) |
| 0x02 | GetEntries | `start u8` (slot index) | `total u8 \| next u8 \| n u8 \| n × (source u8 \| code u16 \| state u8 \| count u32)`. `state` bit0 is occurred and bit1 is active. Only slots with `count > 0` or `active` are listed. `next` is `0xFF` when nothing remains. |
| 0x03 | Clear | `what u8` (bit0 clears the occurred bits, bit1 zeroes the counts) | none |

Result codes: `Ok 0x00`, `InvalidArgument 0x01` (bad length or reserved bits set), `NotSupported 0x02` (unknown subcommand).

## Code table (`components/diag/diag_codes.yaml`)

```yaml
sources:
  system: 0x01
  uart_comm: 0x02
  diag: 0x03
  motor_manager: 0x10
  time_sync: 0x11
  imu: 0x12
codes:
  - source: system
    code: 0x0001
    name: ManagerStartFailed
    severity: error
    kind: record          # record = queued with args; counter = DIAG_COUNT only
    clears: false         # true when the firmware can clear `active` on recovery
    arg0: manager id (1 motor_manager, 2 time_sync, 3 imu)
    arg1: unused
    description: A manager failed to start; it is left out and the rest keep running.
```

`tools/gen_diag_codes.py` reads it and writes three files:
- `components/diag/include/diag_codes.hpp`: a `diag::Source` enum, one `diag::Code` constant per entry carrying its dense slot index, source, code and severity, and a `constexpr` slot table.
- `components/diag/host/diag_codes.py`: a lookup table for the host.
- The code table in `components/diag/README.md`, between marker comments.

The generated files are checked in. CI reruns the script and fails on any diff. Code numbers are never reused. A retired code stays in the YAML marked `retired: true`.

The initial codes are a starting point, and each phase finalizes the codes it needs:
- **system:** ManagerStartFailed, UartCommStartFailed (console only, since there is no link)
- **uart_comm** (counters from `getErrorCounters()`): CrcError, PartialFrame, UartFrameOrParity, RxOverflow, LengthError, DiscardedBytes
- **diag:** RecordDropped, EventTxDropped
- **motor_manager:** ControlOverrun, ControlTimerStalled (clears), TimerRestartFailed, SetpointBadFrame, SetpointRejectedEntry, ConfigQueueFull, MotorCreateFailed (arg0 motor_id, arg1 the failing stage)
- **imu:** SampleLate, DeviceBusy, ReadError, EncodeError, SamplerStalled (clears), FallbackToMock, ModeSwitchFailed, StreamReenableFailed, CalibrationReadFailed, CalibrationApplyFailed, ConfigQueueFull
- **time_sync:** SyncFailed (clears on the next success), ServerErrorStatus, ApplyFailed

## Firmware API (`components/diag`)

```cpp
// Record path: task context only (not ISRs, critical sections or while holding m_mutex).
DIAG_ERROR(code, esp_err, arg0, arg1, "fmt", ...);
DIAG_WARN (code, esp_err, arg0, arg1, "fmt", ...);
// Counter path: real-time safe (an atomic fetch_add plus setting a dirty bit). No log, no queue.
DIAG_COUNT(code);            DIAG_COUNT_N(code, n);
// Active state for codes with `clears: true`.
DIAG_CLEAR(code);            // clears `active`, queues a Cleared event if it was set
```

- **Record path.** It updates the slot (count++, occurred = active = 1) and then calls `xQueueSend(..., 0)` with a 24-byte record. If the queue is full, it counts RecordDropped. If `DIAG_CONSOLE_LOG` is on, it also writes the text line.
- **Storage.** The slot table is `std::array<Slot, kSlotCount>` of atomics in static storage, so nothing is allocated. `diag::init()` (called first in `app_main`) creates the record queue, whose depth is `CONFIG_DIAG_RECORD_QUEUE_DEPTH`. Records written before the reporter starts wait in that queue.
- **Reporter.** `diag::Reporter(UartComm&)` owns a `StoppableTask` and the query registration. Its loop works like this:
  1. Wait on the record queue with a 1 s timeout.
  2. Send each record.
  3. Once a second, read `getErrorCounters()` into the uart_comm slots and send CounterDelta for dirty slots.
  4. Send Cleared events.
  5. A failed `sendCommand` counts EventTxDropped and is never logged through diag, so it can't recurse.
- **Query commands** are answered on the reporter task. The RX callback only queues them, following the existing manager pattern.

### Turning the text logs off at compile time

- `CONFIG_DIAG_CONSOLE_LOG` (bool, default `y`). When it is `n`:
  - The text part of `DIAG_*` expands to nothing, so the format strings are not in the binary and the arguments are not evaluated.
  - Arguments used only in the log are swallowed with `(void)sizeof(...)` so they don't trigger unused-variable warnings (CI builds with `-Werror`).
- When it is `n`, our components also compile with `LOG_LOCAL_LEVEL=ESP_LOG_NONE` through a shared CMake helper. Any remaining `ESP_LOGx` (info, programmer errors) is folded away as well.
- To remove IDF's own logs too, set `CONFIG_LOG_DEFAULT_LEVEL_NONE` / `CONFIG_LOG_MAXIMUM_LEVEL`, and optionally `CONFIG_ESP_CONSOLE_NONE`. IDF's log library code stays linked, because IDF itself uses it.
- CI adds a second `idf.py build` with `sdkconfig.ci.nolog` (`DIAG_CONSOLE_LOG=n`) to check that this configuration builds with zero warnings.

## Phases

### Phase 1: core, reporter, startup (one PR)
1. `components/diag`:
   - Kconfig: event, query and response MSG_TYPEs; queue depth; `DIAG_CONSOLE_LOG`
   - slot table, macros, `init()`, `Reporter`
   - `diag_protocol.cpp` codec
   - README (the spec)
2. `diag_codes.yaml` with the system, uart_comm and diag codes, `tools/gen_diag_codes.py`, and the generated files.
3. Host tests for the codec (each kind, exact lengths, paging, unparsed header) and for slot/latch semantics, if the table logic can be kept free of FreeRTOS.
4. `main.cpp`:
   - `diag::init()` first, then `UartComm`, then the `Reporter`, then the managers.
   - A manager that fails is recorded (ManagerStartFailed) and destroyed, and startup continues.
   - `app_main` never returns.
5. CI: a stale-generated-files check, and the `nolog` build variant.
6. Docs:
   - `CLAUDE.md`: architecture (diag, startup order, IDs `0x23/0x63/0xC0`) and the error-reporting convention ("report through `DIAG_*`; HALs return errors")
   - `components/CLAUDE.md`: logging rules
   - `review/06` E5 marked Fixed

### Phase 2: managers report through diag (one PR)
- motor_manager, imu_bmo_055 and time_sync switch their `ESP_LOGE/W` to `DIAG_ERROR/WARN`.
- Their rate-limited summary logs (overruns, late/busy/read_err/encode_err, setpoint rejects) become `DIAG_COUNT`. The 1 s summary log goes away, because the reporter already rate-limits.
- Recoverable conditions (stalled timers, sync failure) call `DIAG_CLEAR` on recovery.
- `createMotor` reports which HAL stage failed (arg1), and returns `CapacityReached` when the PCNT or MCPWM pool is empty. Part of review E6.
- Remove `IMU_BMO_055_EVENT_MSG_TYPE` and `ManagerConfig::event_msg_type`, and update the IMU README.

### Phase 3: detail byte in configure responses (one PR)
- motor_manager and IMU go to **v4**. A `detail u8` follows `result`, so the motor header grows to 6 bytes and the IMU header to 5. `SET_TIMING` / `SetStreamTiming` applied values move up by one byte.
- Each manager gets its own detail enum: `None, BadLength, UnknownSubcommand, UnknownId, NonFiniteFloat, BadGpio, BadEnum, OutOfRange, QueueFull, HalFailure, ...`.
- A full config queue sends a `Busy` / `InvalidState` response with detail `QueueFull` instead of silently dropping the command. This is sent from the RX callback through the non-blocking `sendCommand`.
- Update the host tests, the READMEs and `protocol-api-guide.md`.

## Out of scope
- Retransmission or ACKs for events.
- Runtime severity filtering, and a replay buffer of recent records (both can be added later as query subcommands).
- Reporting when `UartComm` itself fails (console only, since there is no link).

## Implementation notes (phase 1: where the code differs from the plan above)

- **Startup order.** The Reporter is constructed *after* the managers, not before them, so the `Boot` event carries the final `started_mask`. Records reported during startup wait in the record queue and go out right after `Boot`.
- **GetEntries listing.** A slot is listed when its count is non-zero **or** any state bit is set. A slot whose count was zeroed but whose occurred bit is still set therefore stays visible.
- **Active bit.** Only codes with `clears: true` ever set `active`. Other record codes set only `occurred`, so they never look permanently active.
- **seq.** It counts only event frames that were actually queued for UART TX. When the TX queue is full, events stay in the diag queue and are retried after a 100 ms back-off, so a `seq` gap now means a loss on the link. Losses inside the ESP are counted instead: `diag.RecordDropped` (record queue full), `diag.EventTxDropped` (a query response could not be queued) and `diag.QueryDropped` (query queue full, a code added in phase 1).
- **Codes.** `UartCommStartFailed` has no code, because it can never be sent; it is a console log only. `system.PeriodicSyncStartFailed` was added for the `startPeriodicSync()` failure that used to make `app_main` return.
- **Code namespaces.** The generated constants are `diag::codes::<source>::k<Name>`, with a `diag::Code` struct holding the slot, source, code, severity, kind and `clears`. The macros `static_assert` on kind and severity.
- **time_synced.** time_sync calls `diag::setTimeSynced()` after it first applies the time (time_sync now has a private dependency on diag).
- **Log switch.** The root `CMakeLists.txt` applies `LOG_LOCAL_LEVEL=ESP_LOG_NONE` to every component under `components/` and `main/` when `CONFIG_DIAG_CONSOLE_LOG=n`, so no per-component CMake changes are needed. When disabled, the macros use an `if (false)` call to a printf-checked no-op instead of `(void)sizeof`. The format string and arguments are still type-checked but never emitted or evaluated.
