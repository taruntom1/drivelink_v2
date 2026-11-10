# diag

Reports errors and warnings to the host as **numeric codes over the control UART**, so the host doesn't need to watch the USB-Serial-JTAG console. The console text is still available for bench work and can be compiled out entirely.

- **Push:** `Record`, `CounterDelta`, `Cleared` and `Boot` events on one event MSG_TYPE (default `0xC0`). Each carries a `seq`.
- **Pull:** a query command (default `0x23`, response `0x63`) returns boot info, latched faults and counter totals, and clears them.

There are no ACKs and no retransmission. The host detects a lost event from a gap in `seq` and then queries the latched state.

## Codes

Every code is defined once, in [`diag_codes.yaml`](diag_codes.yaml). `tools/gen_diag_codes.py` generates three files from it:
- [`include/diag_codes.hpp`](include/diag_codes.hpp), which has `diag::codes::<source>::k<Name>`
- [`host/diag_codes.py`](host/diag_codes.py), a lookup table for host tools
- the table at the end of this README

After editing the YAML, run:

```bash
python tools/gen_diag_codes.py
```

Commit the YAML and the generated files together. CI runs the script with `--check` and fails if a generated file is stale. Never renumber or reuse a code; retire it with `retired: true` instead.

A code is one of two kinds:
- **record:** reported with `DIAG_ERROR` / `DIAG_WARN`. It is latched, queued with its `esp_err` and two `u32` args, and sent as a `Record` event. A record code with `clears: true` also has an *active* bit, which `DIAG_CLEAR` resets on recovery (that sends a `Cleared` event).
- **counter:** reported with `DIAG_COUNT`. It is an atomic counter, and changes are sent as `CounterDelta` events at most once per `CONFIG_DIAG_COUNTER_PERIOD_MS`.

## API

```cpp
#include "diag.hpp"
static const char *const TAG = "my_module";   // the macros log with the file's TAG

DIAG_ERROR(diag::codes::system::kPeriodicSyncStartFailed, ret, 0, 0,
           "Periodic time sync not started: %s", esp_err_to_name(ret));
DIAG_WARN (code, ESP_OK, arg0, arg1, "fmt", ...);           // a warning-severity record code
DIAG_COUNT(diag::codes::diag::kQueryDropped);               // real-time safe
DIAG_COUNT_N(code, n);
DIAG_CLEAR(code);                                           // a `clears: true` code's condition is gone
```

| Macro | Context | What it does |
|---|---|---|
| `DIAG_ERROR` / `DIAG_WARN` | Task only | Writes the console line (if enabled). Latches the slot: count+1, occurred, and active if the code clears. Queues a record with the wall-clock time, never waiting. A full queue counts `diag.RecordDropped`. |
| `DIAG_COUNT` / `DIAG_COUNT_N` | Any, including ISRs, critical sections and real-time paths | Two relaxed atomic operations. No log, no queue. |
| `DIAG_CLEAR` | Task only | Clears the active bit, and queues a `Cleared` event if it was set. |

- The macros check at compile time that the code has the right kind and severity, so the code must be a constant.
- Don't call `DIAG_ERROR` / `DIAG_WARN` / `DIAG_CLEAR` on a real-time path (see "Real-time paths" in the root `CLAUDE.md`): they read the clock, post to a queue and may log. Count on the real-time path instead, and let a non-real-time task watch the count and report: motor_manager's config task watches the update task's wake count to report `ControlTimerStalled`.

Other functions:
- `diag::init()`: creates the record queue in static storage, so it can't fail. Call it first in `app_main`. It is idempotent, and `Reporter` calls it too. Records reported before `init()` are counted as `RecordDropped`.
- `diag::setTimeSynced()`: called by time_sync once the wall clock has been set. From then on, events carry the `time_synced` flag.

### Who reports

- Managers (`motor_manager`, `imu_bmo_055`, `time_sync`) and `app_main` report.
- The hardware layers (`motor_hal`, `rotary_encoder_hal`, `rtos_util`, `pid`) don't depend on `diag`. They return `esp_err_t` / `bool`, and the manager that called them reports with its own code and the `esp_err`.
- `uart_comm` doesn't depend on `diag` either. The reporter reads `UartComm::getErrorCounters()` every period and folds it into the `uart_comm` counter codes.

### Reporter

`diag::Reporter(uart_comm::UartComm&, const ReporterConfig&)` registers the query callback and starts one task (`CONFIG_DIAG_TASK_STACK`, `CONFIG_DIAG_TASK_PRIO`). The task does the following:

1. Sends the `Boot` event once.
2. Sends queued records and `Cleared` events, oldest first.
3. Answers queries. The RX callback only queues them and wakes the task.
4. Every `CONFIG_DIAG_COUNTER_PERIOD_MS`, folds in the UART link counters and sends `CounterDelta` for counters that changed.

If the UART TX queue is full, events stay queued and the task tries again after 100 ms. A record is lost only when the diag record queue itself is full (counted as `RecordDropped`). A query response that can't be queued for TX is counted as `EventTxDropped`. `seq` counts only frames that were queued for TX, so a gap means a loss on the link.

Construct the reporter **after** the managers, and set `ReporterConfig::started_mask`, so that `Boot` and `GetInfo` show which managers started. Records from the startup wait in the queue until then. It must not outlive the `UartComm`.

## Kconfig

| Option | Default | Meaning |
|---|---|---|
| `DIAG_EVENT_MSG_TYPE` | `0xC0` | Event MSG_TYPE (0xC0–0xFE) |
| `DIAG_QUERY_MSG_TYPE` | `0x23` | Query MSG_TYPE (0x01–0x3F) |
| `DIAG_QUERY_RESPONSE_MSG_TYPE` | `0x63` | Query response MSG_TYPE (0x40–0x7F) |
| `DIAG_RECORD_QUEUE_DEPTH` | 16 | Records waiting to be sent (static, about 32 B each) |
| `DIAG_QUERY_QUEUE_DEPTH` | 2 | Queries waiting for the reporter task |
| `DIAG_COUNTER_PERIOD_MS` | 1000 | CounterDelta period |
| `DIAG_REPORTER_POLL_MS` | 20 | The longest a record waits before it is sent |
| `DIAG_TASK_STACK` / `DIAG_TASK_PRIO` | 4096 / 4 | Reporter task |
| `DIAG_CONSOLE_LOG` | y | Console text for `DIAG_ERROR` / `DIAG_WARN` (see below) |

### Turning the console text off

When `CONFIG_DIAG_CONSOLE_LOG=n`:
- The text part of `DIAG_ERROR` / `DIAG_WARN` compiles to nothing. The format string is not in the binary, and the arguments are not evaluated, though they are still type-checked.
- The root `CMakeLists.txt` builds every first-party component (everything under `components/` and `main/`) with `LOG_LOCAL_LEVEL=ESP_LOG_NONE`, so their remaining `ESP_LOGx` calls are compiled out as well.
- ESP-IDF's own logs still follow `CONFIG_LOG_DEFAULT_LEVEL`. To remove those too, set `CONFIG_LOG_DEFAULT_LEVEL_NONE`, and optionally `CONFIG_ESP_CONSOLE_NONE`. IDF's log library itself stays linked, because IDF uses it.

The codes are sent over the UART either way. CI builds this configuration from `sdkconfig.ci.nolog`.

## Wire format (protocol v1)

Every field is little-endian. Every body has an exact length. The codec is `priv_include/diag_protocol.hpp`, and the host tests cover it.

### Events (ESP → host): `DIAG_EVENT_MSG_TYPE`

Header, 5 bytes: `version u8 (=0x01) | seq u16 | kind u8 | flags u8`
- `seq` goes up by one for every event frame queued for TX, of any kind, and wraps at 0xFFFF.
- `flags` bit0 is `time_synced`: the wall clock had been set by time_sync when the event was latched. Before that, timestamps count from boot. The other bits are 0.

| kind | Name | Body after the header | Length |
|---|---|---|---|
| 0x01 | Record | `timestamp_us u64 \| severity u8 \| source u8 \| code u16 \| esp_err i32 \| arg0 u32 \| arg1 u32` | 24 |
| 0x02 | CounterDelta | `timestamp_us u64 \| n u8 \| n × (source u8 \| code u16 \| delta u32 \| total u32)` | 9 + 11n |
| 0x03 | Cleared | `timestamp_us u64 \| source u8 \| code u16` | 11 |
| 0x04 | Boot | `timestamp_us u64 \| reset_reason u8 \| started_mask u8` | 10 |

- **Timestamps** are wall-clock µs (`gettimeofday`), the same clock as the motor and IMU streams. A Record or Cleared event is stamped when it is reported, not when it is sent.
- **severity:** 1 = error, 2 = warning.
- **esp_err:** the `esp_err_t` value (`ESP_OK` = 0 when there is none).
- **CounterDelta** lists only counters that changed since the last delta that was sent. When more changed than fit in one frame, it is split across several frames.
- **reset_reason** is the ESP-IDF `esp_reset_reason_t` value: 1 power-on, 3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog, 7 other watchdog, 9 brownout, and so on.
- **started_mask:** bit0 motor_manager, bit1 time_sync, bit2 imu.

### Query (host → ESP): `DIAG_QUERY_MSG_TYPE`, response `DIAG_QUERY_RESPONSE_MSG_TYPE`

- Request header, 3 bytes: `version u8 | seq u8 | subcommand u8`.
- Response header, 4 bytes: `version u8 | seq u8 | subcommand u8 | result u8`. `seq` is echoed from the request.
- A request whose header can't be parsed (too short, or the wrong version) gets `subcommand = 0xFF` and `InvalidArgument`. In that case `seq` is echoed only if the version matched and a seq byte was present.

| Sub | Name | Request body | Response body (after the 4-byte header) |
|---|---|---|---|
| 0x01 | GetInfo | none (0 bytes) | `flags u8 \| uptime_us u64 \| reset_reason u8 \| started_mask u8 \| records_dropped u32 \| events_tx_dropped u32 \| version_len u8 \| version[version_len]` |
| 0x02 | GetEntries | `start u8` (slot index) | `total u8 \| next u8 \| n u8 \| n × (source u8 \| code u16 \| state u8 \| count u32)` |
| 0x03 | Clear | `what u8` | none |

- **GetInfo**
  - `version` is the app version string from `esp_app_get_description()`, at most 32 bytes and not NUL-terminated.
  - `uptime_us` is µs since boot.
- **GetEntries**
  - Lists every code with a non-zero count or a state bit, in slot order, starting at slot `start`.
  - `state` bit0 is *occurred* (sticky until Clear) and bit1 is *active* (cleared by the firmware on recovery).
  - `total` is the number of listed codes in the whole table, and `next` is the `start` for the next page, or `0xFF` when nothing remains.
  - To read the whole table, start at 0 and repeat with `next` until it is `0xFF`.
- **Clear**
  - `what` bit0 clears every occurred bit, and bit1 zeroes every count.
  - Active bits are not touched, because the condition is still there.
  - Zeroing the counts also restarts the CounterDelta baselines.
  - Any other bit gives `InvalidArgument`.

| Result | Meaning |
|---|---|
| 0x00 Ok | |
| 0x01 InvalidArgument | Bad header, wrong body length, or reserved bits set |
| 0x02 NotSupported | Unknown subcommand |

### Host side

- On connect, send `GetInfo`, then page through `GetEntries` from `start = 0`.
- On a `seq` gap, page through `GetEntries` again.
- Decode `(source, code)` with `host/diag_codes.py`. It keeps retired codes, so old logs still decode.

## Code table

<!-- diag-codes:begin (generated by tools/gen_diag_codes.py) -->

| Source | Code | Name | Severity | Kind | Clears | arg0 | arg1 | Description |
|---|---|---|---|---|---|---|---|---|
| `system` (0x01) | 0x0001 | ManagerStartFailed | error | record | no | manager id (1 motor_manager, 2 time_sync, 3 imu) | unused | A manager failed to start. It is left out, and the rest of the firmware keeps running. |
| `system` (0x01) | 0x0002 | PeriodicSyncStartFailed | error | record | no | unused | unused | time_sync started, but its periodic sync task did not. esp_err gives the reason. |
| `uart_comm` (0x02) | 0x0001 | CrcError | warning | counter | no | - | - | A frame candidate was rejected for a CRC mismatch. |
| `uart_comm` (0x02) | 0x0002 | PartialFrame | warning | counter | no | - | - | A partial frame was dropped because the sender stopped mid-frame. |
| `uart_comm` (0x02) | 0x0003 | FrameOrParityError | warning | counter | no | - | - | The UART reported a framing or parity error. |
| `uart_comm` (0x02) | 0x0004 | RxOverflow | warning | counter | no | - | - | The RX hardware FIFO overflowed or the ring buffer was full. |
| `uart_comm` (0x02) | 0x0005 | LengthError | warning | counter | no | - | - | A frame candidate was rejected because LEN exceeded the maximum payload. |
| `uart_comm` (0x02) | 0x0006 | DiscardedBytes | warning | counter | no | - | - | Received bytes that were not part of any valid frame (counts bytes, not events). |
| `diag` (0x03) | 0x0001 | RecordDropped | warning | counter | no | - | - | A record or Cleared event was dropped because the diag record queue was full (or diag was not initialized yet). |
| `diag` (0x03) | 0x0002 | EventTxDropped | warning | counter | no | - | - | A diag query response could not be queued for UART transmission and was dropped. |
| `diag` (0x03) | 0x0003 | QueryDropped | warning | counter | no | - | - | A diag query was dropped because the reporter's query queue was full. |
| `motor_manager` (0x10) | 0x0001 | InitFailed | error | record | no | stage that failed: 1 mutex, 2 config queue, 3 invalid config (UartComm invalid or max_motors 0), 4 configure callback, 5 setpoint callback, 6 update task, 7 control timer, 8 config task | unused | MotorManager failed to start. esp_err gives the reason. system.ManagerStartFailed follows. |
| `motor_manager` (0x10) | 0x0002 | MotorCreateFailed | error | record | no | motor id | hardware stage that failed: 1 motor driver (DIR GPIO or MCPWM), 2 encoder (PCNT) | CreateMotor passed validation, but the hardware could not be set up. ESP_ERR_NOT_FOUND means the MCPWM or PCNT pool is empty and the response is CAPACITY_REACHED. Anything else is answered with INTERNAL_ERROR. |
| `motor_manager` (0x10) | 0x0003 | TimerRestartFailed | error | record | no | requested control period (us) | unused | SET_TIMING could not restart the control timer at the new period. The old period stays in effect. |
| `motor_manager` (0x10) | 0x0004 | ControlTimerStalled | warning | record | yes | wait without a tick (ms) | unused | The control timer stopped waking the update task, so the control loop is not running. Detected by the config task. Cleared when the timer fires again or the manager stops. |
| `motor_manager` (0x10) | 0x0005 | ControlOverrun | warning | counter | no | - | - | Control ticks that were skipped (the update task ran late, or a configure command held the manager mutex). |
| `motor_manager` (0x10) | 0x0006 | SetpointBadFrame | warning | counter | no | - | - | A setpoint stream frame was dropped because its header or length was invalid. |
| `motor_manager` (0x10) | 0x0007 | SetpointRejected | warning | counter | no | - | - | A setpoint stream entry was dropped (unknown motor id, invalid kind, non-finite value, or a full mailbox). |
| `motor_manager` (0x10) | 0x0008 | ConfigQueueFull | warning | counter | no | - | - | A configure command was not applied because the config queue was full. It is answered with detail QueueFull. |
| `motor_manager` (0x10) | 0x0009 | ResponseTxDropped | warning | counter | no | - | - | A configure response could not be queued for UART transmission and was dropped. The command itself was still applied. |
| `time_sync` (0x11) | 0x0001 | InitFailed | error | record | no | stage that failed: 1 UartComm invalid, 2 mutex, 3 response queue, 4 response callback | unused | TimeSync failed to start. esp_err gives the reason. system.ManagerStartFailed follows. |
| `time_sync` (0x11) | 0x0002 | SyncFailed | warning | record | yes | stage that failed: 1 request send, 2 no response before the timeout, 3 host returned an error status, 4 setting the clock | host status byte (stage 3), else 0 | A periodic time sync failed. Reported once per run of failures, and cleared on the next successful sync or when periodic sync stops. A round skipped because a manual sync was in progress is not reported. |
| `time_sync` (0x11) | 0x0003 | BadResponseFrame | warning | counter | no | - | - | A time sync response was dropped because it could not be decoded (bad length or version). |
| `imu` (0x12) | 0x0001 | InitFailed | error | record | no | stage that failed: 1 mutex, 2 invalid config (UartComm invalid, period bounds or operation mode), 3 BNO055 device, 4 default operation mode, 5 config queue, 6 configure callback, 7 config task, 8 stream task, 9 sampling timer | unused | The IMU manager failed to start. esp_err gives the reason. system.ManagerStartFailed follows. |
| `imu` (0x12) | 0x0002 | FallbackToMock | warning | record | no | I2C port | BNO055 I2C address | The BNO055 did not initialize, so the IMU streams synthesized mock data instead (fallback_to_mock_on_init_failure). esp_err gives the device error. |
| `imu` (0x12) | 0x0003 | ModeSwitchFailed | error | record | no | requested operation mode | unused | SetOperationMode could not write the new mode to the BNO055. A busy device (BUSY response) is not reported here. |
| `imu` (0x12) | 0x0004 | StreamReenableFailed | error | record | no | cause: 1 manager mutex busy, 2 sampling timer did not restart | unused | The stream stayed off after an operation-mode switch. Enable it again with SetStreamEnabled. |
| `imu` (0x12) | 0x0005 | CalibrationReadFailed | error | record | no | unused | unused | ReadCalibration failed on the BNO055 bus. A busy device (BUSY response) is not reported here. |
| `imu` (0x12) | 0x0006 | CalibrationApplyFailed | error | record | no | unused | unused | ApplyCalibration failed to write the offsets to the BNO055. A busy device (BUSY response) is not reported here. |
| `imu` (0x12) | 0x0007 | TimerRestartFailed | error | record | no | requested sample period (us) | unused | SetStreamTiming could not restart the sampling timer at the new period. The old period stays in effect. |
| `imu` (0x12) | 0x0008 | SamplerStalled | warning | record | yes | wait without a tick (ms) | unused | The stream is enabled, but the sampling timer stopped waking the stream task. A timeout across a timer restart (enable, SetStreamTiming) is not a stall. Cleared when it fires again, the stream is disabled, or the stream task stops. |
| `imu` (0x12) | 0x0009 | SampleLate | warning | counter | no | - | - | Sample periods that were missed because the stream task ran late. |
| `imu` (0x12) | 0x000A | DeviceBusy | warning | counter | no | - | - | Samples skipped because a configure operation held the BNO055. |
| `imu` (0x12) | 0x000B | ReadError | warning | counter | no | - | - | Samples that failed to read from the BNO055. |
| `imu` (0x12) | 0x000C | EncodeError | warning | counter | no | - | - | Samples that could not be encoded into a stream packet. |
| `imu` (0x12) | 0x000D | ConfigQueueFull | warning | counter | no | - | - | A configure command was not applied because the config queue was full. It is answered with detail QueueFull. |
| `imu` (0x12) | 0x000E | ResponseTxDropped | warning | counter | no | - | - | A configure response could not be queued for UART transmission and was dropped. |

<!-- diag-codes:end -->
