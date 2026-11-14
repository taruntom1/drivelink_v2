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
| `defs.hpp` | The descriptor format: `ItemDesc`, `TypeDesc`, the `ItemKind`, `ValueType`, `ItemFlag`, `LoopClass`, `ActionPlacement` and `BlobDir` enums, `findItem()`, `findType()` and `f32Bits()`. Header-only. |
| `gen/registry.hpp` | **Generated.** Every MSG_TYPE (`defs::msg::k<Name>`), the stream channel ranges (`kSubGroupChannelFirst` / `Count`, `kCmdGroupChannelFirst` / `Count`), the system type id and `kDescriptorHash`, with `static_assert`s on uniqueness, class ranges and channel ranges. |
| `gen/diag_codes.hpp` | **Generated.** The diag sources, codes and slot table, in `defs::diag`. |
| `gen/types.hpp`, `gen/type_<type>.hpp` | **Generated.** The id constants and constexpr descriptor tables of the system type (`gen/type_system.hpp`, `defs::system`, type `0x00`) and of each module type, and `kTypeTable`. No module type is defined yet. |

## Definitions and generated headers

`protocol/defs/` holds the YAML that the `gen/` headers and the generated docs come from: `registry.yaml`, `diag_codes.yaml`, `system.yaml` and, from F3b, one file per module type. `python tools/gen/gen.py` regenerates them, and `--check` fails if they are stale (CI runs it). Never edit a file under `gen/` by hand. The schema, the descriptor format and the hash are described in [docs/protocol/definitions.md](../docs/protocol/definitions.md), and the generated tables are in [docs/protocol/registry.md](../docs/protocol/registry.md).

```cpp
#include "drivelink/protocol/gen/registry.hpp"

constexpr uint8_t type = drivelink::protocol::defs::msg::kDiagQuery; // 0x23
```

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

F3c adds installed CMake exports: configure/build/install this directory, then
`find_package(drivelink_protocol REQUIRED)` and link `drivelink::protocol`.
The exported include paths are relocatable. `package.xml` declares a plain
CMake package for colcon. `host/test/install_smoke.py` checks the protocol and
host exports together using an external consumer.

`protocol/test/` holds the doctest cases for every hand-written header and for the generated registry and descriptor tables (`test_defs.cpp`; the type headers it uses are generated from the generator's test fixtures at build time). They are compiled into the firmware's native test build (`firmware/test/native`, using its vendored doctest) until `protocol/` gets its own test target.

Time protocol v1 is the pure `time_codec.hpp`: exact request/reply codecs and
`time_wire::stampFrame` for a validated encoded reply's TX timestamp and CRC.
It takes the timestamp from its caller and never reads a clock or allocates.
The contract and transport boundaries are in [time.md](../docs/protocol/time.md).

F9 adds allocation-free incremental `Sha256`/`sha256` and `Sha256Digest`, with
known-answer/split-update tests. `ota_image.hpp` preflights complete unsigned
ESP32-S3 application binaries: segment bounds, descriptor magic, checksum and
optional appended digest. It extracts the full ELF build hash; that hash is
distinct from the SHA of the complete upload. IDF remains the authoritative
chip/revision/image validator. These helpers have no OS/firmware dependency.
The OTA family uses [core v8](../docs/protocol/ota.md).
