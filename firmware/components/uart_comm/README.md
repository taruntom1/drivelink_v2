# uart_comm

The UART transport between the host and the core: the IDF UART driver, an RX
task that frames bytes with `drivelink::protocol::Scanner`, a TX task, MSG_TYPE
callbacks, a preallocated command pool and latest-wins stream slots. The frame
format is in `docs/protocol/framing.md`; how the RX and TX paths work inside is
in [CLAUDE.md](CLAUDE.md).

## API (`uart_comm.hpp`)

| Call | Use |
|---|---|
| `UartComm(const UartCommConfig &)`, `isValid()` | Installs the driver and starts both tasks. Check `isValid()`. |
| `registerCallback(type, cb, registration)` | Routes a MSG_TYPE to a callback on the RX task. The `CallbackRegistration` unregisters on destruction and waits for a running callback. |
| `receive(frame, timeout)` | Frames with no callback, from a bounded queue. |
| `sendCommand(type, payload, len, stamp_time)` | Queues a frame from the command pool. `ESP_ERR_NO_MEM` means the pool is empty. `stamp_time` lets TX patch the time-ping timestamp. |
| `openStream(channel, max_len)`, `sendData(channel, payload, len)` | A latest-wins slot per stream channel, opened at setup. `sendData` never blocks or allocates. If the slot is busy, it increments `stream_skipped` and returns `ESP_ERR_TIMEOUT`. |
| `waitTxIdle(timeout_ms)` | Waits until queued TX drains (used before a reboot). |
| `getErrorCounters()`, `stackHighWater()` | Counters and the RX/TX tasks' free stack. |

## Kconfig ("UART Comm Configuration")

Port, TX/RX pins (43/44), baud (921600), `UART_COMM_MAX_PAYLOAD` (1024, which
sizes `RxFrame`), driver ring buffers, command pool and RX queue depths, the
RX/TX task stacks and priorities, the event queue size and the RX timeout.

## Rules

- Callbacks run on the RX task: they must not block or allocate. Hand work off
  through a queue or mailbox.
- Registrations must not outlive the `UartComm`; owners reset them first in
  their destructors.
- An `RxFrame` is about 1 KB. Don't put one on a task stack without checking
  that task's stack mark.
- MSG_TYPEs and stream channels come from the generated registry, never a
  literal or a Kconfig option.
