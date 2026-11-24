# uart_comm

The UART transport between the host and the core: the IDF UART driver, an RX task that frames bytes with `drivelink::protocol::Scanner`, a TX task, MSG_TYPE callbacks, a preallocated command pool and latest-wins stream slots. The frame format is in [framing.md](../../../docs/protocol/framing.md). Transport internals are in [CLAUDE.md](CLAUDE.md), and the firmware-wide rules for callbacks, frame storage and message IDs are in [firmware/CLAUDE.md](../../CLAUDE.md).

## API ([`uart_comm.hpp`](include/uart_comm.hpp))

| Call | Contract |
|---|---|
| `UartComm(config)`, `isValid()` | Installs the driver and starts both tasks. Check `isValid()`; calls on an invalid object that return `esp_err_t` give `ESP_ERR_INVALID_STATE`. The destructor stops both tasks and releases the driver. |
| `registerCallback(type, cb, registration)` | Routes one MSG_TYPE to a callback on the RX task; those frames never reach `receive()`. Resets an active `registration` first. A second callback for a type is `ESP_ERR_INVALID_STATE`; an empty callback is `ESP_ERR_INVALID_ARG`. |
| `CallbackRegistration` | Move-only. `reset()` or destruction unregisters and waits for a running invocation, so captured objects can be destroyed right after. A `reset()` from inside that callback does not wait. Release every registration before destroying the `UartComm`. |
| `receive(frame, timeout)` | Next frame whose type has no callback, from a bounded queue, or `ESP_ERR_TIMEOUT`. A full queue drops the new frame with a warning. |
| `sendCommand(type, payload, len, stamp_time)` | Encodes a command, response or event into a pool buffer without blocking. An empty pool is `ESP_ERR_NO_MEM`. A stream or reserved type ([ranges](../../../docs/protocol/framing.md#msg_type-ranges)), an oversized payload or null data with a nonzero length is `ESP_ERR_INVALID_ARG`. `stamp_time` accepts only a valid time response, whose TX time is patched as its write begins; see [time.md](../../../docs/protocol/time.md#timestamp-boundaries-and-limits). |
| `openStream(channel, max_len)` | Allocates a channel's slot; call once per channel at setup. An open channel is `ESP_ERR_INVALID_STATE`, a non-stream channel or bad `max_len` is `ESP_ERR_INVALID_ARG`, and a failed allocation is `ESP_ERR_NO_MEM`. |
| `sendData(channel, payload, len)` | Replaces the channel's pending value. Real-time safe: two buffers per slot, no blocking, no allocation and no delivery confirmation. A busy slot increments `stream_skipped` and returns `ESP_ERR_TIMEOUT`; an unopened channel is `ESP_ERR_INVALID_STATE`. |
| `waitTxIdle(timeout_ms)` | Config task only: waits until queued command frames reach the wire. Other producers may continue. **A failure must cancel a planned reboot.** |
| `getErrorCounters()`, `stackHighWater()` | The cumulative `ErrorCounters` and the RX and TX tasks' minimum free stack. |

Callbacks run on the RX task with the callback lock held. They must not block or allocate; hand work off through a queue or mailbox. They may register or unregister callbacks, but must not block on a task that is itself waiting to do so.

## Kconfig

"UART Comm Configuration" in [Kconfig](Kconfig) sets the port, pins, baud, payload limit, buffers, queue depths, task stacks and priorities, and RX timeout. Its port and pin defaults are UART1 on GPIO17 (TX) and GPIO18 (RX); the project's [sdkconfig.defaults](../../sdkconfig.defaults) selects UART0 on GPIO43 and GPIO44, the [bench link](../../../docs/bench.md#the-board). `UART_COMM_MAX_PAYLOAD` (default 1024) sizes `RxFrame`, so a frame is about 1 KB. Keep `UART_COMM_TX_BUF_SIZE` above one full frame, or large writes block the TX task.

## Tests

Hardware-in-the-loop tests are in `firmware/test/uart_comm/`; see the [test rules](../../test/CLAUDE.md).
