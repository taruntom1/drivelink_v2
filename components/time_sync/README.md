# time_sync Component

`time_sync` is a protocol layer built on top of `uart_comm` for synchronizing ESP32 system time to a PC time server over UART.

## Design Goals

- Reuse `uart_comm` framing, CRC, callback dispatch, and thread-safe send path.
- Provide request/response style time synchronization.
- Support both manual sync and periodic sync from a FreeRTOS task.
- Estimate and expose time offset, last round-trip time, and last sync moment.
- Optionally enable NTP-style round-trip compensation.

## Wire Protocol (payload inside uart_comm frame)

Default message types:

- Request: `CONFIG_TIME_SYNC_REQUEST_MSG_TYPE` (default `0x20`)
- Response: `CONFIG_TIME_SYNC_RESPONSE_MSG_TYPE` (default `0x60`)

Request payload (`12` bytes, little-endian):

- `version` (`u8`) = `1`
- `flags` (`u8`) = `0` (reserved)
- `request_id` (`u16`)
- `client_unix_send_us` (`i64`)

Response payload (`30` bytes, little-endian):

- `version` (`u8`) = `1`
- `status` (`u8`) (`0` means success)
- `request_id` (`u16`) echoes request
- `echoed_client_unix_send_us` (`i64`) echoes request field
- `server_unix_rx_us` (`i64`) server receive timestamp
- `server_unix_tx_us` (`i64`) server transmit timestamp
- `reserved` (`u16`)

All timestamps are Unix epoch microseconds (UTC).

## Offset Estimation

Let:

- `t0` = client send time (ESP)
- `t1` = server receive time (PC)
- `t2` = server send time (PC)
- `t3` = client receive time (ESP)

With compensation enabled:

$$
\theta = \frac{(t1 - t0) + (t2 - t3)}{2}
$$

$$
\delta = (t3 - t0) - (t2 - t1)
$$

- `theta` is applied as client offset estimate (`server - client`).
- New ESP time is set to `t3 + theta`.
- `delta` is stored as last round-trip estimate.

With compensation disabled:

- Offset estimate is `t2 - t3`.

## Public API

Header: `include/time_sync.hpp`. The default request/response MSG_TYPEs (`kDefaultRequestMsgType`, `kDefaultResponseMsgType`, from Kconfig) are in `include/time_sync_types.hpp`. The wire codec (`time_sync_protocol.hpp`) is private.

- `TimeSync(uart_comm::UartComm&, const TimeSyncConfig&)`
- `bool isValid() const`
- `esp_err_t triggerManualSync(bool wait_for_response = true, TickType_t timeout = pdMS_TO_TICKS(250))`
- `esp_err_t startPeriodicSync()`
- `void stopPeriodicSync()`
- `bool isPeriodicSyncRunning() const`
- `int64_t getEstimatedOffsetUs() const`
- `uint32_t getLastRoundTripUs() const`
- `int64_t getLastServerUnixUs() const`
- `uint64_t getLastSyncMonotonicUs() const`

## Example

```cpp
#include "uart_comm.hpp"
#include "time_sync.hpp"

extern "C" void app_main(void)
{
    uart_comm::UartComm uart({});
    if (!uart.isValid()) {
        return;
    }

    time_sync::TimeSyncConfig cfg{};
    cfg.sync_interval_ms = 5000;
    cfg.response_timeout_ms = 300;
    cfg.enable_round_trip_compensation = true;

    time_sync::TimeSync sync(uart, cfg);
    if (!sync.isValid()) {
        return;
    }

    sync.triggerManualSync(true, pdMS_TO_TICKS(300));
}
```

## PC Time Server Expectations

The PC side should:

1. Parse time sync request payload.
2. Capture receive timestamp (`server_unix_rx_us`) as close as possible to payload parse time.
3. Capture transmit timestamp (`server_unix_tx_us`) immediately before sending response.
4. Echo `request_id` and `client_unix_send_us`.
5. Return status `0` for success.

## Notes

- The component does not own UART and must be passed an already initialized `uart_comm::UartComm` instance.
- Callback execution is in `uart_comm` RX task context; keep callback logic short.
- Same-instance concurrent `triggerManualSync` calls are serialized internally.

## Errors reported to the host

time_sync reports through the `diag` component (events on `0xC0`, see `components/diag/README.md`). Source `time_sync`:

- `InitFailed` (error): the constructor failed. `arg0` is the stage: 1 UartComm invalid, 2 mutex, 3 response queue, 4 response callback.
- `SyncFailed` (warning, clears): a periodic sync failed. It is reported once per run of failures and cleared by the next successful sync. `arg0` is the stage (1 send, 2 no response before `response_timeout_ms`, 3 the host answered with a non-zero status, 4 setting the clock), and `arg1` is the host's status byte for stage 3. A periodic round skipped because a manual sync held the lock is not a failure. `SyncFailed` is also cleared when periodic sync stops. Manual syncs (`triggerManualSync`) return their error to the caller and log a console warning with the stage; they are not reported through diag.
- `BadResponseFrame` (counter): a response that could not be decoded was dropped on the RX task.

After the first successful sync, every diag event carries the `time_synced` flag.

## Kconfig

Under `Time Sync Configuration` in `menuconfig`, these defaults can be changed:

- `TIME_SYNC_REQUEST_MSG_TYPE`
- `TIME_SYNC_RESPONSE_MSG_TYPE`
- `TIME_SYNC_TASK_STACK` (default 4096): default `TimeSyncConfig::task_stack_size` for the periodic sync task
- `TIME_SYNC_TASK_PRIO` (default 6): default `TimeSyncConfig::task_priority`
