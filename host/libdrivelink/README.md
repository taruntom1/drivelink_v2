# libdrivelink

C++23 (GCC 13.1 minimum), standalone Asio serial/TCP and synchronous/asynchronous typed
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
v9 and sets the device's maximum receive payload; the scanner's hard limit is
1024. A different descriptor hash is accepted because module definitions are
dynamic. It is not a core protocol-version mismatch.

`SessionOptions` configures baud (921600), request timeout (1500 ms), read
retries (2), event capacity (128), request/completion capacity (64), a FIFO
dispatcher and periodic time ownership (enabled). TCP endpoints currently require a numeric
IPv4 address. Serial endpoints are `serial://COM3` or `serial:///dev/ttyUSB0`.
`close()` is idempotent, cancels pending calls and joins the I/O thread.
It can overlap cancellation and admission of both single and batched typed
requests; no application dispatcher progress is needed to finish closing.
Windows keeps the adapter's DTR/RTS lines disabled: reopening the CLI must not
reset the ESP32 and discard pending layout edits. Asio's Windows open already
configures no flow control; its flow_control(none) setter is deliberately
avoided because it reasserts those bootstrap lines.
Windows USB-UART drivers may briefly retain exclusive port ownership after a
CLI exits or briefly lose the device name while reenumerating. Opening retries
AccessDenied/FileNotFound at 25 ms intervals up to the session timeout; no
protocol request has been sent during this bounded wait.

Synchronous requests serialize across callers except emergency DISARM;
asynchronous requests can overlap. Replies match type/seq/family/op.
Timeouts permanently retire that sequence ID for this connection. Reconnect
when all 256 IDs are retired. Only known read operations retry, with a new ID;
mutations never retry. After a write timeout query the device before deciding
whether to repeat it. Successful request IDs wrap normally.

Streams/events are copied into a bounded queue. `nextEvent` drains it; overflow
drops oldest and increments `droppedEvents()`. `Client::event` validates and decodes
status, core events and all four diag event kinds into an `Event` variant. The client does not reconnect or replay
configuration automatically. Boot invalidates pending requests, managed groups
and the clock fit. `DeviceModel` reloads automatically when the link stays open
(serial/PTY); a TCP vdev reboot closes its connection, so reconnect explicitly.

`Client` requires one caller. Results are named C++ structs in
[`types.hpp`](include/drivelink/types.hpp): descriptors, instances, device info,
param/action results, group configuration, capture, testgen, OTA and diagnostics.
`Event` distinguishes status, capture/testgen completion, diagnostic record,
counter, cleared and Boot events; unknown message types retain a raw `Frame`.
`DeviceModel` provides the discovered snapshot, explicit reload and event-driven
runtime state described below.

## Serial ports

```cpp
#include <drivelink/ports.hpp>
for (const auto &port : drivelink::listSerialPorts())
    std::cout << port.endpoint << ' ' << port.description << '\n';
```

`listSerialPorts()` lists present ports in natural order (COM3 before COM10)
with the USB VID/PID, product text and serial number when known: SetupAPI on
Windows, `/sys/class/tty` on Linux (legacy platform `ttyS*` placeholders are
skipped). It never opens a port, because opening an ESP auto-reset adapter can
pulse DTR/RTS and reset the board; whether a port is busy shows when opening
it fails (`Error::os_error`). The sysfs parser is pure and tested
(`ports_detail::listSysfs`).

## Device identity

File helper `readOtaImage(path)` takes a UTF-8 path and opens the native
filesystem path, including Unicode directories on Windows.

`DeviceInfo::device_id` is the factory MAC as six bytes; `name` is a UTF-8
string, empty by default. `Client::setName(std::string_view)` accepts 0..16
bytes without C0/C1 controls, applies live (also armed), and returns a typed
`SystemResult`. Local validation precedes submission; sent mutations never
retry. SAVE persists and REVERT restores the name. Names and params commit
in the same A/B layout generation. Call `reload()`/`reloadAsync()` after rename
or REVERT to refresh `DeviceSnapshot::info`; Boot reload includes identity.
The id stays stable across rename, reboot and reset; names need not be unique.

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
codes, list both layouts, read all parameters, and query diagnostic entries. Safe mode is readable:
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

`snapshot()` returns an independent value; `snapshotShared()` returns an
immutable shared snapshot which remains valid after later publications. Reads
are thread-safe. The model retains its Session, not its construction Client;
keep the Session alive longer than the model. Never load synchronously on I/O.
`reload()` and `reloadAsync()` serialize complete metadata loads. Failure retains
the previous complete metadata. Reads span several device requests, not one
device transaction: coordinate other writers when a stable view is required.
Call an explicit reload after service mutations; the library never replays them.
After a SET or a layout op, `refreshConfigAsync()` re-reads only the configured
and pending layouts and every parameter (instances and system) with the
descriptors already loaded; status and diagnostics are kept. It needs a ready
snapshot of the current boot: a Boot or a full reload in between fails it
without publishing, and the Boot reload replaces it.

Status, core and diagnostic events update runtime fields: `status`, diagnostic
entries, capture/testgen completion, `connected`, `generation`, `ready` and
`reloading`. Status includes arm state, last disarm reason, pending reboot,
NVS dirty, loop overruns and capture/testgen flags. Coalesced diagnostic counts
and active state are reconciled by queries. `onChange()` receives Model, Status,
Diagnostic, ArmState, Link or ReloadFailed through the session dispatcher.
`notification_losses` and `malformed_events` expose incomplete event delivery.

Boot marks `ready=false`, invalidates runtime state and starts a complete reload;
only a matching generation can publish. Failed reloads report ReloadFailed and
leave `ready=false`; callers can retry explicitly. Check both `ready` and
`connected` before using a model for operations. A late successful load cannot
restore a lost connection. These observers do not consume `nextEvent()`.
High-rate subscription groups are pulled from buffers, not pushed to observers.

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
copying into `std::expected<T, Error>` preserves them. Diagnostic
queries use family zero. `peer_version` is set when the peer answered with an
incompatible core/diag wire version (the message is unchanged), so a client
can show both versions. `os_error` carries the operating-system code of a
failed open (Win32 `ERROR_ACCESS_DENIED` for a serial port another program
holds). `boot_changed` marks an observed Boot invalidation,
distinct from malformed protocol input. OTA readiness may restart its reads
after that marker; it never repeats REBOOT. A submitted mutation that times out, disconnects or
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
and the arming gate still apply. The managed command API below owns keepalives; the caller still owns arming.
The low-level `command` API retains its explicit finite-slot behavior.

`commandPayload` is a pure encoder. `decodeSubscription(frame, group,
descriptors)` checks exact lengths and converts values in configured order;
it returns `std::nullopt` for a stale epoch/channel. Resolve descriptors from the device.
Reconfigure/reload group metadata explicitly after a mutation. The bounded
Session event queue carries subscriptions as well as status and diagnostics.
Pulled buffers and mappings are described below; legacy event consumers retain
their existing behavior.

Arming wrappers use core v10: arm(), disarm() and acknowledgeFault(). Session
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
Session owns periodic pings on I/O: first at 100 ms, then every 200 ms for
three seconds, then every second. Explicit pings can share an in-flight ping.
`clockModel()` returns the aged fit; `generation()` identifies its device boot.
Set `periodic_time=false` for explicit measurement or future agent-owned time.
The CLI uses explicit pings only, preserving its measurement cadence.

`MappedTime` includes device microseconds, boot generation, mapped host time,
uncertainty and validity. Status/diagnostic/Boot/subscription results expose it
as `time`; capture status maps its trigger and `CaptureData::times` maps regular
sample rows. Irregular captures have invalid reconstructed row mappings. Raw
Frame/Reply metadata retains its receive-time clock and generation, so old data
cannot be relabelled after Boot. `ClockModel::uncertainty()` includes half the
minimum RTT, fit residual/extrapolation and age. It is an estimate, not a bound
on unknown constant path asymmetry. Timestamp boundaries and wire lengths are in
[time.md](../../docs/protocol/time.md).

Windows serial reads deliberately override Asio 1.30.2's 1 ms inter-byte-gap
timeout: continuous telemetry could hold a ping until the 2048-byte buffer
filled. The documented Win32 first-byte mode returns available bytes promptly,
with a 1000 ms idle timeout. It preserves write timeout values and DTR/RTS.
Stale-frame expiry and cancellation remain independent. See the linked time
spec for the Win32 reference and measured regression.

## Asynchronous services

```cpp
auto request = client.async(
    [refs = std::vector<drivelink::Ref>{{0, 0, 4}}](drivelink::Client &worker) {
        return worker.get(refs);
    },
    [](std::expected<drivelink::ParamResult, drivelink::Error> result) {
        // Runs through the session dispatcher, never on I/O or inline.
    });
// request.cancel(); // stops waiting; does not undo an accepted mutation
```

`Client::async` runs the callable on a bounded serial service executor with a
fresh Client, reusing every synchronous typed parser and OTA workflow. Own all
arguments in the callable; do not retain the worker Client or borrowed spans.
Use SDK calls and bounded computation; never wait for another service task or
dispatcher callback from inside that executor. A model can be constructed there
and returned as `std::shared_ptr<DeviceModel>` because it retains only Session.

`Completion<T>` is a move-only callback receiving `std::expected<T, Error>`.
Supply `SessionOptions::dispatcher` to enqueue tasks FIFO on the application
thread/strand, or use the default serial callback executor. Keep the dispatcher
and callback state alive until all accepted completions run. Every accepted
operation completes exactly once, including refusal, timeout, cancellation and
close. Raw/typed completions preserve reply order even while a typed parser is
finishing; composite services use their final awaited reply. Admission includes
callbacks awaiting consumption. Overload throws Usage before acceptance;
accepted completions are never dropped. Callback exceptions do not stop later
delivery. Handle destruction does not cancel.

`asyncRequest`, `asyncDiagRequest` and `asyncTimePing` expose the raw paths.
`disarmAsync` is the typed emergency path: it bypasses the service executor,
reserves request/completion/TX admission and gets the next TX opportunity after
the active frame. Synchronous waits use internal promises and do not require
the application event loop. Transport failure completes work but keeps I/O
alive until explicit close, so racing submissions cannot become stranded.

Descriptors follow their cursor one page at a time. Capture READ and OTA WRITE
have at most four chunks in flight, preflight payload/resources, and correlate
out-of-order replies. OTA retains its identical-offset/data retry exception.
Async cancellation never confirms or repeats REBOOT; if an upload is interrupted,
inspect OTA status and abort explicitly when cleanup could not complete.

## Tuning math (`drivelink/tuning.hpp`, H2e-1)

`stepMetrics(t, response, step_time, from, to, options)` measures the response
to one step of the target, relative to the step size so a negative step works
the same. Rise time runs from the first 10 % crossing to the first 90 %
crossing (interpolated between samples); overshoot is the peak beyond `to` in
% of the step (0 when none); settling time runs from the step to the last
entry into the band (default 2 % of the step); steady-state error is `to`
minus the mean response over the tail (default the last 10 % of the samples
after the step). `end_time` drops samples after it (a stopped or cut run).
Each metric is optional: `problem` says why one is missing (no step, too few
samples after the step, the response never reaches 90 %, it never settles).
NaN samples are skipped. Pure and device-free; the GUI's Tune page uses it.

## OTA workflow (`drivelink/ota.hpp`)

`Client::otaUpload` reports `OtaProgress` (acknowledged offset, size and how
many chunks were sent again with identical data). The rest of an update:

- `describeOtaImage(bytes)` / `loadOtaImage(path)`: a validated unsigned
  ESP32-S3 app with its app descriptor (version, project, date, time, IDF),
  ELF SHA-256 (the build identity) and the whole-file SHA-256.
- `requestOtaReboot(client)`: the single REBOOT. A reply is acceptance only; a
  timeout, lost link or interrupting Boot returns `Uncertain`. Never call it
  twice for one update.
- `classifyOtaBoot(candidate, info, status)`: pure. `Confirmable`/`Confirmed`
  only when the uploaded ELF runs from the other slot as the boot slot, idle,
  with the matching GET_INFO build id; `NotCommitted` (old slot, image Ready or
  Error), `RolledBack` (old slot again, idle and confirmed), else `Mismatch`.
- `confirmOtaCandidate(client, candidate, confirm)`: GET_INFO (core version
  check), STATUS, classification and, only for `Confirmable` with `confirm`,
  one CONFIRM followed by a STATUS recheck of the identity. A CONFIRM whose
  outcome stays unknown throws Timeout with `outcome_unknown` and the OTA
  CONFIRM family/op.
- `otaUpdate(endpoint, options, image, update, progress, cancelled)`: the
  CLI's `ota-upload` on its own sessions, with the 30 s reconnect window and the
  CLI's recovery messages. Within that window it reconciles an uncertain
  CONFIRM by reconnecting and checking again (CONFIRM is idempotent).

## Pulled groups and managed commands

```cpp
#include <drivelink/buffers.hpp>
auto configured = client.subConfigure(group, loop, divider, refs);
auto buffer = session.subscribeBuffer(group, configured.epoch, refs.size(),
                                      2048, configured.generation);
drivelink::GroupSample sample;
if (buffer->latest(sample)) { /* fixed-size raw slots and mapped timestamp */ }
while (buffer->pop(sample)) { /* bounded ring, drop oldest on overflow */ }
```

`latest()` and `pop()` are noexcept and do not allocate or take a mutex. Atomic
payload words make the seqlock valid under the C++ memory model. Each seqlock
copy tries at most eight times; ring advancement is also bounded. Contention or
no data returns false. `dropped()` counts ring
overflow; wire counter gaps are separate. `malformed()` counts invalid frames.
Wrong epochs/channels are ignored. Pass the configuration generation when
attaching; Boot, replacement and clear invalidate old buffers. Do not retain
wire frames across epoch reuse or reboot.

Include `<drivelink/command.hpp>` and call
`client.managedCommand(group, loop, input_refs, finite_hold_bits)` while disarmed.
It discovers input descriptors, reads the watchdog and starts prefix-only
keepalives on I/O at one third of its interval. It never arms automatically.
One producer calls `write(span<double>)` or `writeBits(span<uint32_t>)`; both
are bounded, allocation-free and noexcept, returning false on invalid values,
concurrent producers or a full queue. NaN in `write` maps to the supplied finite
hold (for example a consumer's measured position); infinity is rejected.
`writeBits` remains strict. The consumer supplies all semantics and can update
finite holds through `hold()` on its producer thread.

Call `discardQueued()` before a mode-change request. It invalidates producer
queue entries and pending TX value frames; already-sent frames cannot be recalled.
Keepalives never replay a value or satisfy the firmware's fresh-command gate.
`stop()`, destruction, Boot, reconfiguration and link loss stop managed traffic.
`rejected()` counts local producer refusals. Call DISARM explicitly for an
immediate stop; a stopped keepalive otherwise leaves the device watchdog active.
