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
v4 and sets the device's maximum receive payload; the scanner's hard limit is
1024. A different descriptor hash is accepted because module definitions are
dynamic. It is not a core protocol-version mismatch.

`SessionOptions` configures baud (921600), request timeout (1500 ms), read
retries (2) and event capacity (128). TCP endpoints currently require a numeric
IPv4 address. Serial endpoints are `serial://COM3` or `serial:///dev/ttyUSB0`.
`close()` is idempotent, cancels pending calls and joins the I/O thread.
Windows keeps the adapter's DTR/RTS lines disabled: reopening the CLI must not
reset the ESP32 and discard pending layout edits. Asio's Windows open already
configures no flow control; its flow_control(none) setter is deliberately
avoided because it reasserts those bootstrap lines.
Windows USB-UART drivers may briefly retain exclusive port ownership after a
CLI exits or briefly lose the device name while reenumerating. Opening retries
AccessDenied/FileNotFound at 25 ms intervals up to the session timeout; no
protocol request has been sent during this bounded wait.

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
`drivelink::drivelink`. It finds `drivelink_protocol`, Threads and nlohmann_json
3.11.3 EXACT: JSON is part of the public ABI, so another patch/minor version
cannot be substituted without rebuilding the library and its consumers.
Asio headers stay private. `package.xml` lets colcon discover the CMake package;
the superbuild is ignored by colcon. No project license grant has been added.

Group services are `groups`, `subConfigure`, `cmdConfigure`, `subEnable` and
`groupClear`. Mutations return epoch/channel; LIST includes all slots and refs.
`command` queues typed slot bits with an epoch/counter; an empty list is a
keepalive. It never waits for a peer and has no stream acknowledgement. The
bounded queue can refuse a send. Retain the session until writes drain; the
short-lived CLI uses an ordered read before closing. Device command validation
and the arming gate still apply. F4a sends no automatic keepalives or ARM.

`commandPayload` is a pure encoder. `decodeSubscription(frame, group,
descriptors)` checks exact lengths and converts values in configured order;
it returns null for a stale epoch/channel. Resolve descriptors from the device.
Reconfigure/reload group metadata explicitly after a mutation. The bounded
Session event queue carries subscriptions as well as status and diagnostics.
The cached, lock-free device/time model remains H1 work.

Arming wrappers use core v4: arm(), disarm() and acknowledgeFault(). Session
routes system DISARM through the fast-path MSG_TYPE. Group capacities are
read-only system params; Client::groups retains convenience capacity fields.
