# time_sync

## Overview
`time_sync` synchronizes ESP32 wall clock time against a host time server over `uart_comm` request/response messages. It supports manual sync and periodic background sync with optional NTP-style RTT compensation.

## Features
- Manual one-shot synchronization
- Optional periodic sync task
- Protocol encode/decode helpers for request/response payloads
- Offset, RTT, and last-sync metrics
- Round-trip compensation toggle

## Dependencies
Required:
- `uart_comm`
- `esp_timer`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_TIME_SYNC_REQUEST_MSG_TYPE` | hex | `0x20` | Time-sync request message type. |
| `CONFIG_TIME_SYNC_RESPONSE_MSG_TYPE` | hex | `0x60` | Time-sync response message type. |

## Public API
Public header: `include/time_sync.hpp`

- `TimeSync(uart_comm::UartComm&, const TimeSyncConfig&)`
- `bool isValid() const`
- `esp_err_t triggerManualSync(bool wait_for_response=true, TickType_t timeout=...)`
- `esp_err_t startPeriodicSync()` / `void stopPeriodicSync()` / `bool isPeriodicSyncRunning() const`
- State getters: `getEstimatedOffsetUs`, `getLastRoundTripUs`, `getLastServerUnixUs`, `getLastSyncMonotonicUs`

Protocol helpers (`time_sync_protocol.hpp`):
- `encodeRequest`, `decodeRequest`, `encodeResponse`, `decodeResponse`

## Usage Example
```cpp
#include "uart_comm.hpp"
#include "time_sync.hpp"

extern "C" void app_main(void)
{
    uart_comm::UartComm uart({});
    if (!uart.isValid()) return;

    time_sync::TimeSync sync(uart, {});
    if (!sync.isValid()) return;

    sync.triggerManualSync(true, pdMS_TO_TICKS(250));
}
```

## Error Handling
- Invalid state -> `ESP_ERR_INVALID_STATE`
- Timeout waiting for response -> `ESP_ERR_TIMEOUT`
- Server status non-zero -> `ESP_ERR_INVALID_RESPONSE`
- Time application failure -> `ESP_FAIL`

## Thread Safety
- Sync operations are serialized with `sync_mutex`.
- State getters lock `state_mutex`.
- Callback publishes decoded responses to queue.

## Limitations & Known Issues
- Requires host-side responder implementing matching protocol.
- `settimeofday` capability/behavior is platform-dependent.
