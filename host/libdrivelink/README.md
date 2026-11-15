# libdrivelink

C++23 (GCC 13.1 minimum), standalone Asio serial/TCP and synchronous typed
services for every current core family, status and diagnostic v3 decoding.
Public headers contain no Qt, ROS, Asio or JSON types.
It uses device-supplied descriptors; it contains no module-specific code.

```cpp
#include <drivelink/client.hpp>
drivelink::Session session("tcp://127.0.0.1:8765");
drivelink::Client client(session);
auto info = client.info(); // version check and payload-cap negotiation
auto types = client.types(); // whole descriptors, all pages
```

Call `info()` before issuing other service calls. A successful info checks core
v8 and sets the device's maximum receive payload; the scanner's hard limit is
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
drops oldest and increments `droppedEvents()`. `Client::event` validates and decodes
status, core events and all four diag event kinds into an `Event` variant. The client does not reconnect or replay
configuration automatically. Reconnect after a reboot and call `info()` again.

`Client` requires one caller. Results are named C++ structs in
[`types.hpp`](include/drivelink/types.hpp): descriptors, instances, device info,
param/action results, group configuration, capture, testgen, OTA and diagnostics.
`Event` distinguishes status, capture/testgen completion, diagnostic record,
counter, cleared and Boot events; unknown message types retain a raw `Frame`.
`DeviceModel` provides the snapshot and explicit reload described below;
event-driven updates remain H1b.

## Device snapshot

```cpp
#include <drivelink/model.hpp>
drivelink::Session session("tcp://127.0.0.1:8765");
drivelink::Client client(session);
drivelink::DeviceModel model(client); // GET_INFO, discovery, layouts, params
const auto previous = model.snapshot(); // independent value copy
model.reload(); // replaces the model only after every read succeeds
```

Construction and `reload()` negotiate the payload through `Client::info()`,
check the core version, discover types, items, enum entries and diagnostic
codes, list both layouts, and read all parameters. Safe mode is readable:
`snapshot().safeMode()` reports it without starting loops or exiting safe mode.
Descriptor hash differences are accepted; the device supplies the metadata.

`DeviceSnapshot` contains:

- `info`, `types` keyed by type ID (each has `items` keyed by item ID), and `codes`.
  Each `ModelItem` retains its descriptor and discovered enum entries, including
  units, ranges, flags, source/wiring metadata and action arguments/results.
- `system_params`, keyed by item ID. This includes every `limits.*` parameter;
  the system has no layout entry or separate pending copy.
- `configured` and `pending`, keyed by `InstanceKey{type, instance}`. Each entry
  has the instance record and descriptor-typed `params`, keyed by item ID.
  IDs may have gaps. The pending view includes staged values and labels, added
  instances and removals; without pending edits it is the effective current view.

Signals, inputs and actions have descriptors, but no values in this snapshot.
Use `valueBits(value)` to recover a parameter's exact slot representation.
Malformed discovery (including duplicate IDs and undescribed enum values) fails
explicitly. Parameter reads obey both the 64-ref limit and the negotiated payload.

The model, Client and its Session are single-caller from the model's perspective;
keep them alive and never invoke the model on the I/O thread. It consumes no
events and performs no mutations or automatic refresh. Call `reload()` after
service mutations. Errors propagate without replacing any part of the previous
snapshot. Successful reload invalidates references into its old contents; retain
a value copy when needed. Reads span multiple requests, so publication is atomic
on the host, not a device-wide transaction: coordinate other writers if a stable
device view is required during the load. Boot/event updates remain H1b.

`parseValue(descriptor, text, enums)` parses into an exact 32-bit wire slot.
`decodeValue(ValueType, bits)` returns a `Value` variant (bool, u8/u16/u32,
i32, f32 or a distinct `EnumValue`); `valueBits(value)` performs the reverse
conversion. Enum entries are discovered from the device. Conversion rejects
narrowing, invalid booleans and non-finite floats. Range clamping remains on
the firmware. Param/action/capture wire results preserve raw slot bits; apply
the corresponding discovered descriptor when interpreting them. GET/SET accept
atomic batches of at most 64 refs. SET returns actual applied bits and the
pending-reboot result. `CaptureData` holds a typed `status` and sample rows;
`CaptureStatus::trigger_index` is optional. OTA digests are 32-byte arrays.

```cpp
const drivelink::Ref ref{0, 0, 4}; // core/system watchdog parameter
auto descriptor = client.item(ref);
auto result = client.get({&ref, 1});
drivelink::Value value = drivelink::decodeValue(descriptor.value_type, result.values.at(0));
```

Sync calls throw `Error` (usage, connection, timeout, rejection or protocol).
`DeviceError` represents a device refusal, retaining result/detail and request
family/op. Request context and `outcome_unknown` live in the base `Error`, so
copying into a future `std::expected<T, Error>` preserves them. Diagnostic
queries use family zero. A submitted mutation that times out, disconnects or
gets a malformed response is uncertain; query the device before deciding to
repeat it. Queued writes may still reach the peer. Local preflight failures
remain certain. Read classification is separate from retry eligibility:
testgen STATUS remains non-retried, but never implies a mutation. OTA cleanup
preserves error metadata and failed reconciliation retains uncertainty.

The package installs headers, a static library and `drivelinkConfig.cmake`.
Consumers use `find_package(drivelink REQUIRED)` and link
`drivelink::drivelink`. Only `drivelink_protocol` and Threads are package
dependencies; Asio stays private. JSON formatting and its pinned dependency
belong to the CLI. Installed-consumer tests explicitly disable JSON package
discovery. `package.xml` lets colcon discover the CMake package; the superbuild
is ignored by colcon. The root proprietary LICENSE applies.

Group services are `groups`, `subConfigure`, `cmdConfigure`, `subEnable` and
`groupClear`. Mutations return epoch/channel; LIST includes all slots and refs.
`command` queues typed slot bits with an epoch/counter; an empty list is a
keepalive. It never waits for a peer and has no stream acknowledgement. The
bounded queue can refuse a send. Retain the session until writes drain; the
short-lived CLI uses an ordered read before closing. Device command validation
and the arming gate still apply. Automatic keepalives remain H1b work; the caller owns arming.

`commandPayload` is a pure encoder. `decodeSubscription(frame, group,
descriptors)` checks exact lengths and converts values in configured order;
it returns `std::nullopt` for a stale epoch/channel. Resolve descriptors from the device.
Reconfigure/reload group metadata explicitly after a mutation. The bounded
Session event queue carries subscriptions as well as status and diagnostics.
Lock-free buffers, async calls and time mapping remain H1b.

Arming wrappers use core v8: arm(), disarm() and acknowledgeFault(). Session
routes system DISARM through the fast-path MSG_TYPE. Group capacities are
read-only system params; Client::groups retains convenience capacity fields.

## Time v1 (F8)

`Session::timePing()` returns `TimeSample` with host I/O-thread send/receive,
device RX/TX, echoed u16 ID and a process-local clock generation. It does not
retry. Failed IDs retire until reconnect; close and Boot wake pending callers.
Generation changes across sessions/Boot reset a reused `ClockEstimator`.

Include `<drivelink/time.hpp>` for the pure single-caller estimator. Construct
once, call `add(sample)`, then `model(monotonicUs())`. Defaults select lowest
RTT per 5 s bin in a 120 s bounded window; eight samples over 30 s enable drift.
Models expire after 5 s and expose age/RTT/residual/fit span. `model.map(esp_us)`
maps to host monotonic microseconds; drift is host-us/device-us, not UTC.
No periodic thread or application-data mapping is installed. Timestamp
boundaries, asymmetry limits and exact wire lengths are in
[time.md](../../docs/protocol/time.md).

Windows serial reads deliberately override Asio 1.30.2's 1 ms inter-byte-gap
timeout: continuous telemetry could hold a ping until the 2048-byte buffer
filled. The documented Win32 first-byte mode returns available bytes promptly,
with a 1000 ms idle timeout. It preserves write timeout values and DTR/RTS.
Stale-frame expiry and cancellation remain independent. See the linked time
spec for the Win32 reference and measured regression.
