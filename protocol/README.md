# Shared protocol library

## Source map

| Area | Implementation |
|---|---|
| Transport | [Frame construction and validation](include/drivelink/protocol/framing.hpp), [CRC functions](include/drivelink/protocol/crc.hpp), and [incremental receive scanner](include/drivelink/protocol/scanner.hpp). The [framing specification](../docs/protocol/framing.md) defines byte layout, rejection order and golden frames. |
| Payload codecs | [Field reader and writer](include/drivelink/protocol/wire_codec.hpp), [core messages](include/drivelink/protocol/core_codec.hpp), [device identity](include/drivelink/protocol/identity_codec.hpp), and [time messages and transmit timestamp patching](include/drivelink/protocol/time_codec.hpp). |
| Device descriptors | [Handwritten descriptor types](include/drivelink/protocol/defs.hpp) represent the [YAML definitions](defs/). The [generated headers](include/drivelink/protocol/gen/) contain the message registry, diagnostic codes and per-type tables, including motor, IMU and mock modules. See the [definitions schema](../docs/protocol/definitions.md). |
| Firmware image preflight | [SHA-256](include/drivelink/protocol/sha256.hpp) hashes upload bytes. The [ESP image parser](include/drivelink/protocol/ota_image.hpp) validates image structure and extracts the ELF identity. ESP-IDF performs final device-side validation. |

## Using the scanner

Keep one scanner per receive stream across reads. In this example, the transport supplies `read_into` (returns the number of bytes written, bounded by capacity) and `consume` (processes or copies the frame before returning).

```cpp
drivelink::protocol::Scanner scanner(max_payload);

auto drain = [&] {
    drivelink::protocol::Scanner::Frame frame;
    while (scanner.next(frame)) {
        consume(frame);
    }
};

auto on_readable = [&] {
    std::size_t capacity = 0;
    auto* dst = scanner.writable(capacity);
    scanner.commit(read_into(dst, capacity));
    drain();
};

auto on_partial_frame_timeout = [&] {
    if (scanner.dropStale()) {
        drain();
    }
};
```

The caller owns timeout detection. Drain again after dropping a stale candidate because the receive window may already contain a complete following frame.

## Build integration

ESP-IDF discovers this directory through the firmware’s `EXTRA_COMPONENT_DIRS`. The standalone [CMake build](CMakeLists.txt) exports a position-independent C++20 static library. The [colcon manifest](package.xml) declares a plain CMake package. An installed consumer links it with:

```cmake
find_package(drivelink_protocol REQUIRED)
target_link_libraries(my_target PRIVATE drivelink::protocol)
```

## Verification

The [firmware native test target](../firmware/test/native/CMakeLists.txt) compiles the [protocol tests](test/) and generates descriptor fixtures from [test YAML](../tools/gen/tests/fixtures/) at build time. The [installation smoke test](../host/test/install_smoke.py) installs the protocol and host exports, then builds an external host-library consumer through `find_package`.
