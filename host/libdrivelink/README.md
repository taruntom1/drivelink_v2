# libdrivelink

First host slice: C++20, standalone Asio serial/TCP, framed sessions and core
system/layout/describe/param/action services, status and diagnostic v3 decoding.
It uses device-supplied descriptors; it contains no module-specific code.

```cpp
#include <drivelink/client.hpp>
drivelink::Session session("tcp://127.0.0.1:8765");
drivelink::Client client(session);
auto info = client.info(); // version check and payload-cap negotiation
auto types = client.types(); // whole descriptors, all pages
```

Call `info()` before issuing other service calls. A successful info checks core
v1 and sets the device's maximum receive payload; the scanner's hard limit is
1024. A different descriptor hash is accepted because module definitions are
dynamic. It is not a core protocol-version mismatch.

`SessionOptions` configures baud (921600), request timeout (1500 ms), read
retries (2) and event capacity (128). TCP endpoints currently require a numeric
IPv4 address. Serial endpoints are `serial://COM3` or `serial:///dev/ttyUSB0`.
`close()` is idempotent, cancels pending calls and joins the I/O thread.

Session requests serialize across callers; replies match type/seq/family/op.
Timeouts permanently retire that sequence ID for this connection. Reconnect
when all 256 IDs are retired. Only known read operations retry, with a new ID;
mutations never retry. After a write timeout query the device before deciding
whether to repeat it. Successful request IDs wrap normally.

Streams/events are copied into a bounded queue. `nextEvent` drains it; overflow
drops oldest and increments `droppedEvents()`. `Client::event` validates/decode
status and all four diag event kinds. The client does not reconnect or replay
configuration automatically. Reconnect after a reboot and call `info()` again.

`Client` requires one caller. Its service results are nlohmann JSON values;
`Reply::result`/`detail` retain device failures. `Error` distinguishes usage,
connection, timeout, rejected operation and malformed/incompatible protocol.
`parseValue`/`valueJson` convert scalar slots using wire descriptor types.
Integers never narrow silently; floats reject non-finite values. Range clamping
is left to the firmware, and SET returns the actual applied bits and a
PendingReboot outcome where appropriate. `Client::get/set` accept atomic batches
of at most 64 refs; the caller controls their domains.

The package installs headers, a static library and `drivelinkConfig.cmake`.
Consumers use `find_package(drivelink REQUIRED)` and link
`drivelink::drivelink`. It finds `drivelink_protocol`, Threads and nlohmann_json.
Asio headers stay private. `package.xml` lets colcon discover the CMake package;
the superbuild is ignored by colcon. No project license grant has been added.
