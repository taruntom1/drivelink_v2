# libdrivelink

| Caller needs | API |
|---|---|
| Own a serial/TCP connection, receive events, track time or close outstanding work | [Session](include/drivelink/session.hpp) |
| Issue a typed device operation or submit it for asynchronous completion | [Client](include/drivelink/client.hpp), [completion and cancellation](include/drivelink/async.hpp), [result types](include/drivelink/types.hpp) |
| Discover modules and follow configured, pending and runtime state across updates | [DeviceModel](include/drivelink/model.hpp) |
| Pull high-rate telemetry or supply commands from a control loop | [Subscription buffers](include/drivelink/buffers.hpp), [managed commands](include/drivelink/command.hpp) |
| Prepare an image, update a device or reconcile an interrupted update | [OTA workflow](include/drivelink/ota.hpp) |

Installed CMake consumers use `find_package(drivelink REQUIRED)` and link `drivelink::drivelink`. For changes to execution or lock ownership, see the [library rules](CLAUDE.md).

## Open and discover

```cpp
#include <drivelink/client.hpp>

drivelink::Session session("tcp://127.0.0.1:8765");
drivelink::Client client(session);
auto info = client.info();
auto types = client.types();
```

Call `info()` first to check the peer's core protocol version and negotiate the payload limit. Discover descriptors from the device even when its descriptor hash differs. Use discovered item types and enum entries to interpret raw result slots. SET returns the actual applied values and whether they require reboot.

A direct Session accepts serial endpoints and numeric IPv4 TCP endpoints. [Port enumeration](include/drivelink/ports.hpp) lists adapters without opening them. A busy port is detected on open. For an agent connection, resolve a typed hostname with `resolveTcpEndpoint` and set `SessionOptions::agent` to an `AgentHello` identifying the device and client. Use `listAgentDevices` for a lobby query. [Agent discovery](include/drivelink/discovery.hpp) blocks for its browse window, so call it off the UI thread. [Agent API](include/drivelink/agent.hpp) · [Agent contract](../../docs/protocol/agent.md)

## Requests, failure and recovery

Use each synchronous Client from one caller and keep the Session alive for its dependents. Synchronous operations throw [Error](include/drivelink/error.hpp). A device refusal carries result, detail and request context. Inspect `peer_version` for a version mismatch, `os_error` for an open failure and `boot_changed` for an observed reboot.

| Situation | Caller action |
|---|---|
| Eligible idempotent read times out | The library can retry with a fresh sequence ID. Some reads, including test-generator STATUS, have no automatic retry. |
| Submitted mutation times out, disconnects or receives a malformed reply | Inspect `outcome_unknown` and reconcile with device state before deciding whether another action is needed. A local preflight failure occurred before submission. |
| OTA WRITE needs retransmission | Repeat only identical bytes at the same offset. Reconcile uncertain BEGIN, FINISH and CONFIRM through STATUS. Issue REBOOT once. |
| Ordinary core request IDs are exhausted | Reconnect. By default a timed-out ID stays retired for the connection. Long-lived agent links can opt into reuse after `retire_quarantine`. Emergency DISARM can use a retired ID when no free ID remains. |

For asynchronous work, capture inputs by value and use the temporary Client only inside the callable. Waiting there for another service task or dispatcher callback can deadlock. Supply a FIFO application dispatcher or use the default callback executor. An accepted operation delivers exactly one completion, including cancellation or close. Keep the dispatcher and callback state alive through delivery. Overload can reject admission before a handle is returned. Destroying a handle leaves the operation running. Explicit cancellation stops waiting and cannot undo a submitted mutation. Use `disarmAsync()` for emergency disarm. Session close is idempotent and can finish without pumping the application dispatcher.

## Snapshot readiness and event loss

```cpp
#include <drivelink/model.hpp>

drivelink::DeviceModel model(client);
auto snapshot = model.snapshotShared();
if (snapshot->connected && snapshot->ready) {
    // Read this immutable generation's descriptors, layouts and parameters.
}
```

Model construction negotiates and loads discovery data. The model retains the Session reference beyond construction, so the Session must outlive it. A shared snapshot stays valid after later publications. Discovery spans several device requests. Coordinate writers if a stable cross-request view is required.

After SET or a layout change, `refreshConfigAsync()` refreshes configured/pending layouts and parameter values against the current boot's descriptors. Use a full reload after rename or REVERT to refresh identity. Boot invalidates readiness and managed traffic. The model reloads if the link remains open. After disconnect, reconnect explicitly. A retained snapshot alone cannot establish that the current device is ready.

| Delivery path | How to detect incomplete data |
|---|---|
| Session events | `nextEvent()` drains the bounded queue. Overflow drops the oldest frame and increments `droppedEvents()`. Observers do not consume this queue. |
| Agent events | `nextAgentEvent()` has a separate bounded queue and `droppedAgentEvents()`. Device telemetry cannot evict lease or closing events from this queue. |
| Model notifications | Inspect `notification_losses`, `malformed_events` and reload failures. Reconcile the current generation before acting on stale state. |
| Subscription buffers | `dropped()` counts ring overflow and `malformed()` counts invalid frames. Track wire counter gaps separately. |

## Telemetry and command producers

Attach a buffer using the epoch and generation returned by configuration:

```cpp
auto configured = client.subConfigure(group, loop, divider, refs);
auto buffer = session.subscribeBuffer(group, configured.epoch, refs.size(),
                                      2048, configured.generation);
drivelink::GroupSample sample;
while (buffer->pop(sample)) {
    // Consume the sample before the next pop overwrites it.
}
```

Use `latest()` for a recent sample or `pop()` for ordered draining. Both are bounded and can return false on contention or no data. Boot, replacement and clear invalidate the old attachment. For low-level command sends, successful queuing is local acceptance. The stream has no peer acknowledgement.

Create a managed command group while disarmed and supply finite hold values. One producer writes values or raw bits. Check the return value for local refusal. A NaN passed to `write()` selects the supplied hold, infinity is rejected, and `writeBits()` requires valid bits. Arm explicitly. Before a mode change, call `discardQueued()` to invalidate producer and pending TX values. Already-transmitted frames cannot be recalled.

By default the Session sends prefix-only keepalives at one third of the watchdog interval. Set `keepalive=false` when loss of the producer loop must stop watchdog feeding, as in ros2_control. A keepalive does not supply a fresh setpoint. Stopping managed traffic leaves the watchdog active. Request DISARM explicitly for an immediate stop.

## Time, OTA and tuning

- **Time mapping.** Check validity, uncertainty and boot generation before using mapped host-monotonic times. Buffered data retains its receive-time generation. Irregular captures cannot reconstruct valid per-row timestamps. Agent sessions compose the agent clock fit with the device model. Disable periodic time when taking explicit measurements. [Time API](include/drivelink/time.hpp) · [Time contract](../../docs/protocol/time.md)
- **Firmware updates.** `Client::otaUpload` uploads and prepares. Use `otaUpdate` for the complete workflow, or classify the running candidate explicitly before confirmation. A reboot reply acknowledges acceptance. Verify the running ELF identity, boot slot and negotiated core version, confirm only the matching candidate, then recheck. After cancellation or uncertain cleanup, inspect STATUS before deciding whether to abort. [OTA API](include/drivelink/ota.hpp) · [Recovery contract](../../docs/protocol/ota.md)
- **Offline tuning.** Use `stepMetrics` for step-response measurements, `fitFopdt` for open-loop identification and `simcPi` for suggested gains. Check the optional metrics, fit quality, settling and problem fields before consuming results. [Tuning API](include/drivelink/tuning.hpp)
