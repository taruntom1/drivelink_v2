# protocol

The wire library shared by the firmware and (from F3c on) the host software. Pure C++20: no ESP-IDF, FreeRTOS or OS dependency, and no allocation after construction.

Under ESP-IDF it is a component (the firmware lists `protocol/` in `EXTRA_COMPONENT_DIRS`). Elsewhere it builds as a static library, `drivelink::protocol`. The package export and `package.xml` for colcon come with the first host consumer (F3c).

## Contents

All headers are under `include/drivelink/protocol/`, namespace `drivelink::protocol`.

| Header | What |
|---|---|
| `wire_codec.hpp` | `wire::Reader` and `wire::Writer`: bounds-checked little-endian field codec with a sticky error flag. Header-only. |
| `crc.hpp` | `crc16Ccitt()`: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), table-driven. |
| `framing.hpp` | Frame constants (`kStartByte`, `kMaxPayload`, ...), `frameSize()`, the MSG_TYPE ranges and `isCommand/isResponse/isStream/isEvent`, `encodeFrame()`, `validateCrc()`. |
| `scanner.hpp` | `Scanner`: finds frames in a byte stream and resyncs after noise. |

## Frame (current)

`AA AA AA | MSG_TYPE u8 | LEN u8 | payload | CRC16 (little-endian)`. The CRC covers MSG_TYPE, LEN and the payload. F1b changes this to a `u16` LEN with a header CRC8.

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
- A candidate with a bad LEN or CRC drops one byte and the scan resumes. A false `AA AA AA` inside a payload can't hide a real frame behind it.
- `Scanner` has no clock and no logging. The caller decides when a partial frame is stale and what to log. `counters()` reports CRC errors, length errors, timeouts and discarded bytes.
- Frame payloads are views into the window (no copy). Copy what you need before the next call.

## Tests

`protocol/test/` holds the doctest cases for every header. They are compiled into the firmware's host test build (`firmware/test/host`, using its vendored doctest) until `protocol/` gets its own test target.
