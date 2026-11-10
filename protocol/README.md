# protocol

The wire library shared by the firmware and (from F3c on) the host software. Pure C++20: no ESP-IDF, FreeRTOS or OS dependency, and no allocation after construction.

Under ESP-IDF it is a component (the firmware lists `protocol/` in `EXTRA_COMPONENT_DIRS`). Elsewhere it builds as a static library, `drivelink::protocol`. The package export and `package.xml` for colcon come with the first host consumer (F3c).

## Contents

All headers are under `include/drivelink/protocol/`, namespace `drivelink::protocol`.

| Header | What |
|---|---|
| `wire_codec.hpp` | `wire::Reader` and `wire::Writer`: bounds-checked little-endian field codec with a sticky error flag. Header-only. |
| `crc.hpp` | `crc16Ccitt()`: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), and `crc8()`: CRC-8 (poly 0x07, init 0x00), both table-driven. |
| `framing.hpp` | Frame constants (`kStartByte`, `kMaxPayload` = 1024, `kOverhead` = 9, ...), `frameSize()`, the MSG_TYPE ranges and `isCommand/isResponse/isStream/isEvent`, `encodeFrame()`, `headerPayloadLen()`, `headerCrcValid()`, `validateCrc()`. |
| `scanner.hpp` | `Scanner`: finds frames in a byte stream and resyncs after noise. |

## Frame

`AA AA AA | MSG_TYPE u8 | LEN u16 | HCRC u8 | payload | CRC16`, little-endian. HCRC is `crc8()` over MSG_TYPE and LEN; the CRC16 covers MSG_TYPE, LEN, HCRC and the payload. [docs/protocol/framing.md](../docs/protocol/framing.md) is the spec, with golden frames.

## Using the scanner

```cpp
drivelink::protocol::Scanner scanner(max_payload);

size_t space = 0;
uint8_t *dst = scanner.writable(space);   // fill up to `space` bytes
scanner.commit(bytes_written);

drivelink::protocol::Scanner::Frame frame;
while (scanner.next(frame)) {
    // frame.msg_type, frame.len, frame.payload: valid until the next scanner call
}
// no frame and no new bytes for a while:
scanner.dropStale();
```

Rules:
- At each preamble: the HCRC first (as soon as the 4 header bytes are in), then LEN against the limit, then the CRC16 once the frame is complete. A candidate that fails drops one byte and the scan resumes. A false `AA AA AA` inside a payload can't hide a real frame behind it, and a false preamble is rejected without waiting for its payload.
- `Scanner` has no clock and no logging. The caller decides when a partial frame is stale and what to log. `counters()` reports CRC errors, header CRC errors, length errors, timeouts and discarded bytes.
- The window is allocated once (two maximum-size frames). Rejecting a byte only advances a read offset; the unconsumed bytes are moved to the front in `writable()`.
- Frame payloads are views into the window (no copy). Copy what you need before the next call.

## Tests

`protocol/test/` holds the doctest cases for every header. They are compiled into the firmware's host test build (`firmware/test/host`, using its vendored doctest) until `protocol/` gets its own test target.
