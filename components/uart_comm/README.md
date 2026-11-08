# uart_comm Component

`uart_comm` is a lightweight framed UART transport for ESP-IDF.
It wraps raw UART bytes with:

- Start marker: `0xAA 0xAA 0xAA`
- Header: `MSG_TYPE (1 byte)` + `LEN (1 byte)`
- Payload: `0..CONFIG_UART_COMM_MAX_PAYLOAD` bytes
- CRC: CRC-16/CCITT over `MSG_TYPE + LEN + PAYLOAD` (little-endian on wire)

The component is designed to carry two traffic styles on one UART link:

- Command/response/event traffic: reliable queued delivery, FIFO, high TX priority
- Stream traffic: latest-wins per channel, low TX priority, low latency updates

## Public API (What to Use and When)

Header: `components/uart_comm/include/uart_comm.hpp`

### `sendCommand(msg_type, payload, len)`
Use this for discrete messages that must be handled in order:

- Commands (`0x01..0x3F`)
- Responses (`0x40..0x7F`)
- Events (`0xC0..0xFE`)

Behavior:

- Enqueued into an internal TX command queue (FIFO)
- Returns `ESP_OK` on success
- Returns `ESP_ERR_NO_MEM` if TX command queue is full
- Returns `ESP_ERR_INVALID_ARG` if you pass a stream type (`0x80..0xBF`)

Use this when dropping intermediate messages is not acceptable.

### `sendData(channel, payload, len)`
Use this for continuous telemetry/control streams (`0x80..0xBF`).

Behavior:

- Updates one stream slot per channel (latest value overwrites older unsent value)
- TX task sends stream slots after draining all pending commands
- Fire-and-forget (`void`), length is clamped to max payload

Use this when only the newest value matters (for example sensor snapshots or periodic control state).

### `registerCallback(msg_type, callback, registration)`
Use this when a specific message type should be handled immediately in RX task context.

Behavior:

- Callback is invoked directly from the internal RX task
- Frames of that `msg_type` are not pushed into `receive()` queue
- The callback stays registered while the `CallbackRegistration` object is alive. Destroying it, or calling `reset()`, unregisters the callback. Unregistering waits for a callback that is already running, so after `reset()` returns it is safe to destroy whatever the callback captured.
- Only one callback per `msg_type`: registering a second one returns `ESP_ERR_INVALID_STATE`
- Every registration must be released before the `UartComm` is destroyed. If one is still active, the destructor logs an error and aborts. In practice, declare the registration after the `UartComm` it belongs to, or reset it in the owner's destructor.

Use this for event-like handling or low-latency processing.
Keep callbacks short and non-blocking.

### `receive(frame, timeout)`
Use this for pull-based message handling in your app task.

Behavior:

- Returns frames only for message types without a registered callback
- Returns `ESP_OK` if a frame was dequeued
- Returns `ESP_ERR_TIMEOUT` on timeout

Use this for main-loop style command dispatch.

### `getErrorCounters()`
Use this for diagnostics. Counters include:

- `crc_errors`
- `timeouts`
- `uart_errors`
- `overflows`

## Message Type Ranges

- `0x01..0x3F`: Commands
- `0x40..0x7F`: Responses
- `0x80..0xBF`: Stream channels
- `0xC0..0xFE`: Events

`0x00` and `0xFF` are outside the defined ranges and should be avoided.

## Minimal Usage Example

```cpp
#include "uart_comm.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace uart_comm;

extern "C" void app_main(void)
{
    UartCommConfig cfg{}; // defaults from Kconfig
    UartComm comm(cfg);

    if (!comm.isValid()) {
        // initialization failed (UART config/driver/task setup)
        return;
    }

    // Low-latency stream callback for channel 0x80.
    // stream_cb unregisters it when destroyed (declared after comm, so it goes first).
    CallbackRegistration stream_cb;
    comm.registerCallback(0x80, [](const RxFrame& f) {
        // Keep callback brief; runs in RX task context
        (void)f;
    }, stream_cb);

    // Send a command (queued, FIFO)
    const uint8_t cmd_payload[] = {0x10, 0x20};
    comm.sendCommand(0x01, cmd_payload, sizeof(cmd_payload));

    // Send latest telemetry sample (latest-wins per stream channel)
    const uint8_t stream_payload[] = {0xAA, 0x55};
    comm.sendData(0x80, stream_payload, sizeof(stream_payload));

    // Poll for non-callback frames
    RxFrame rx{};
    while (true) {
        if (comm.receive(rx, pdMS_TO_TICKS(1000)) == ESP_OK) {
            // Handle rx.msg_type / rx.payload
        }
    }
}
```

## Configuration (menuconfig)

Component settings are under:

- `UART Comm Configuration`

Key options:

- UART selection: port, TX pin, RX pin, baud rate
- Protocol sizing: max payload, pattern queue size
- Buffers/queues: RX/TX ring buffers, command queue depth, RX queue depth
- Task tuning: RX/TX task stack and priority
- RX timeout symbols

All fields in `UartCommConfig` default to Kconfig values.

## Integration Notes

- The component CMake already declares required dependencies (`driver`, `esp_driver_uart`).
- Add `REQUIRES uart_comm` to any component that includes `uart_comm.hpp`.
- Typical ESP-IDF flow after config changes:
  - `idf.py menuconfig`
  - `idf.py build`

## Operational Notes

- Internal tasks:
  - RX task parses frames using UART hardware pattern detection (`3 x 0xAA`)
  - TX task prioritizes command queue, then transmits dirty stream slots
- If a callback is registered for a message type, that type bypasses `receive()` queueing.
- Same-instance concurrent API calls are protected for internal shared state, but application-level ordering semantics are still your responsibility.

## Choosing the Right Pattern Quickly

- Request/response protocol: use `sendCommand` + `receive`
- Asynchronous event handling by type: use `registerCallback`
- High-rate periodic values where only newest matters: use `sendData`
- Link health and debug telemetry: use `getErrorCounters`
