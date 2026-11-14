# CLAUDE.md (uart_comm)

How the UART transport works. The rules for code that *uses* it (callbacks, `sendData`, `RxFrame` sizes) are in [firmware/CLAUDE.md](../../CLAUDE.md). The frame format is specified in `docs/protocol/framing.md`. Paths are relative to the repository root.

## Frame and limits

- Frame layout: `AA AA AA | MSG_TYPE | LEN u16 | HCRC | payload | CRC16`. HCRC is a CRC-8 over MSG_TYPE and LEN. The CRC16 (CCITT-FALSE) covers MSG_TYPE, LEN, HCRC and the payload. Encoding, CRCs and scanning come from `protocol/`.
- `MAX_PAYLOAD` is 1024 (`CONFIG_UART_COMM_MAX_PAYLOAD`, 128 to 1024; the protocol limit is `drivelink::protocol::kMaxPayload`). It sizes `RxFrame`.
- The default baud rate is 921600, and the UART ISR runs from IRAM (`CONFIG_UART_ISR_IN_IRAM`). The driver's RX and TX ring buffers are 4096 bytes, above one full frame.
- MSG_TYPE ranges: commands `0x01–0x3F`, responses `0x40–0x7F`, streams `0x80–0xBF`, events `0xC0–0xFE`.

## RX

- Framing is done in software. Hardware pattern detection is not used.
- `Parser` is the driver side. Driver events only wake the RX task, and `Parser::nextFrame()` pulls bytes from the ring buffer into a `drivelink::protocol::Scanner`, which scans for the preamble.
- The preamble is not escaped, so a rejected candidate (bad HCRC, LEN or CRC16) drops one byte and rescans. The HCRC and LEN are checked before the payload is awaited, so a false preamble doesn't hold up RX.
- **Never flush the RX buffer:** it may already hold the next valid frames.
- The stale-frame timer and the logging stay in `Parser`. The scanner has no clock.
- A received frame either goes to the callback registered for its MSG_TYPE, on the RX task, or into a queue read by `receive()`.

## TX

Both paths use only buffers allocated in the constructor or in `openStream()`.

- `sendCommand()` takes a free frame buffer from a preallocated pool (`CONFIG_UART_COMM_CMD_QUEUE_DEPTH` buffers), encodes into it and queues its index. The TX task drains the queue first. `ESP_ERR_NO_MEM` means the pool is empty.
- `sendData()` writes to a per-channel stream slot where only the latest value is sent.
  - A channel is opened once at setup with `openStream(channel, max_len)`, which allocates the slot (two buffers of `max_len`).
  - `sendData()` never blocks or allocates. It try-locks the slot, copies into the buffer the TX task isn't sending, and returns `ESP_ERR_TIMEOUT` if the slot was busy (counted in `ErrorCounters::stream_skipped`).
- `UartComm::stackHighWater()` reports the RX and TX tasks' free stack, for logging.

## Message ids

- Message ids and stream channels come from the generated registry (`protocol/defs/registry.yaml`, listed in `docs/protocol/registry.md`), through `<drivelink/protocol/gen/registry.hpp>`: `drivelink::protocol::defs::msg::k<Name>`.
- The generator and the header's `static_assert`s check that values are unique, in their class range, and clear of the group channel ranges.

## Tests

The hardware-in-the-loop tests are in `firmware/test/uart_comm/`; see [firmware/test/CLAUDE.md](../../test/CLAUDE.md).

F8 adds `sendCommand(..., stamp_time=true)`, accepted only for exact time-v1
responses. Each pool entry owns its stamp metadata, set before publishing its
ready index and cleared before recycling. The TX task patches the timestamp
and CRC immediately before driver submission. Do not add deferred callbacks,
hold a caller's pointer, drain UART for a ping, or patch ordinary frames.
Driver/FIFO backlog remains measurement uncertainty (`docs/protocol/time.md`).
