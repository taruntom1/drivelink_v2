# core_os

The firmware core and virtual device use the same [public adapter headers](include/core_os/). The [IDF component](CMakeLists.txt) and [native test build](../../test/native/CMakeLists.txt) select backend sources and the corresponding private state types at build time. Callers supply task settings and resource limits.

## Scheduling and callback lifetime

| Adapter | ESP-IDF | Native |
|---|---|---|
| [PeriodicTask](include/core_os/periodic_task.hpp) | A timer wakes a dedicated task. Notification counts include missed periods. Real-time tasks register with the Task WDT before the timer starts, feed it after callbacks, and unregister on stop. Stop fences the timer before joining the task. | A worker waits on deadlines and passes the elapsed period count. Stop and period changes wake it. Affinity is observable by tests but does not constrain the desktop scheduler. |
| [PeriodicTimer](include/core_os/periodic_timer.hpp) | The callback runs on the ESP timer task, independently of loop progress. Stop fences callbacks before releasing their context. | A separate thread runs callbacks under the real-time guard. Stop joins the worker. |
| [OneShotTimer](include/core_os/one_shot.hpp) | Cancellation stops a pending timer. A callback already running can outlive cancellation. Its context must stay valid until it finishes. | Cancellation wakes and joins the worker, including a callback already in progress. |
| [Event](include/core_os/event.hpp) and [clock](include/core_os/clock.hpp) | A static binary semaphore coalesces signals. A positive timeout rounds up to ticks with an extra tick for the minimum delay. Time comes from the monotonic ESP timer. | A flag and condition variable coalesce signals. Signalling briefly acquires the event mutex. Time comes from a steady clock. |

The owner serializes task and timer lifecycle calls. On ESP32-S3, a task started with no affinity can become pinned after using the FPU. The [bench affinity observer](backend/idf/include/core_os/bench_affinity.hpp) exposes initial and post-callback placement in instrumented builds. Read observations outside the real-time path.

## Tick-safe operations and instrumentation

A loop tick can read the clock, signal an event, attempt a [mutex](include/core_os/mutex.hpp) once, increment diagnostic counters, raise or clear **latched** conditions, and query active safety flags. Record reporting and clearing a **record** condition belong on a task outside the tick. The [diagnostic sink](include/core_os/diag_sink.hpp) accepts explicit instance identity. Install the single-device disarm observer before enabling loops. Clear it after stopping them so the configuration caller fences in-flight notifications before releasing the callback context.

Use the [atomic capability check](include/core_os/atomics.hpp) for shared words. It accounts for IDF's out-of-line word atomics on PSRAM builds. Keep shared atomic storage in internal RAM. See the [firmware real-time rules](../../CLAUDE.md) for allocation and blocking constraints.

[RealtimeScope](include/core_os/realtime.hpp) instruments native execution. It counts C++ allocation, project C allocator calls routed through GNU linker wrappers, blocking mutex acquisition, event waits and KV operations. Native tests can install a [violation sink](backend/native/include/core_os/native_realtime.hpp). IDF implements the guard as a no-op. Vdev installs [bounded diagnostic sinks](backend/native/include/core_os/native_diag.hpp) before starting tasks and detaches them after joining. Native counters use fixed atomic storage.

## Storage, reset and image state

| Adapter | ESP-IDF | Native |
|---|---|---|
| [KvStore](include/core_os/kv_store.hpp) | Opens NVS handles per call and commits each write or erase. A missing read leaves the namespace absent. Enumeration collects namespaces before invoking the caller. On exhausted pages or an incompatible NVS version, initialization erases the partition, retries, and reports the erase. | In-memory by default. Attaching an [exclusive file store](backend/native/include/core_os/native_file.hpp) before boot makes each blob durable by flushing and atomically replacing it. A multi-blob save still relies on the core's generation commit. |
| [Boot information](include/core_os/boot_info.hpp) | Panic and interrupt/task watchdog resets count as crashes. RTC no-init words retain the count across resets and validate it with a magic value and complement. | Process state survives Core reconstruction. An attached boot file carries reset state across vdev processes. |
| [Capture memory](include/core_os/capture_memory.hpp) | Capacity is capped by the requested limit and largest free PSRAM block, or a 32 KiB internal-memory allowance on boards without PSRAM. Allocate the ring at configuration time. | Heap-backed with an adjustable capacity and an injectable allocation failure. |
| [OTA](include/core_os/ota.hpp) | The app-update API writes sequentially to the inactive slot. Finish validates the upload. Select commits boot metadata. Confirm marks the pending running image valid. Adapter errors preserve the IDF cause. | Isolated in-memory slots by default. Vdev can persist selection, pending verification and rollback in atomically replaced metadata. Prepared uploads expire on reset. Corrupt metadata fails closed. The simulation exercises boot state and identity without executing the image. |

## Native test controls

- [Scheduling and allocation](backend/native/include/core_os/native_task.hpp) supports manual loop and monitor advancement, requested-affinity observation, and injected task-start, timer-restart and capture-allocation failures. Change scheduling mode or the start observer only while owners are stopped.
- [KV faults](backend/native/include/core_os/native_kv.hpp) distinguish permanent write failure, a single failed write, and a truncated or corrupted blob whose write reports success. A read observer sees the state at each `get`. [Boot controls](backend/native/include/core_os/native_boot.hpp) provide reset and reboot observations.
- [OTA faults](backend/native/include/core_os/native_ota.hpp) can fail before an operation or after durable boot selection. Use the latter to exercise recovery when the caller receives an error after the commit.
