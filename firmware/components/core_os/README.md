# core_os

Thin OS adapters for the pure `core`. The core uses only these, `protocol/` and the C++ standard library, so it builds both on the device and natively (`firmware/test/native`, later the virtual device).

## Backends

| Backend | Where | Built by |
|---|---|---|
| ESP-IDF | `backend/idf/` | this component's `CMakeLists.txt` |
| native | `backend/native/` | `firmware/test/native/CMakeLists.txt` |

The API headers in `include/core_os/` are common to both. Each backend has its own `core_os/detail/backend_types.hpp` (the state the API types hold) in `backend/<name>/include`, and the build picks the backend by include dir and sources. There is no `#ifdef` in the API headers.

## API (namespace `core_os`)

| Header | What |
|---|---|
| `types.hpp` | `Err` (0 = `kOk`; `esp_err_t` values on the device, -1 natively), `TickFn`, `kNoAffinity` |
| `clock.hpp` | `nowUs()`: monotonic µs since boot (`esp_timer_get_time` / `steady_clock`) |
| `mutex.hpp` | `Mutex` (`lock`, `tryLock`, `unlock`) and `LockGuard`. Static storage. |
| `event.hpp` | `Event`: a binary signal. `signal()` never blocks and is safe from a loop tick; `wait(timeout_us)` takes it or times out (at least `timeout_us`); `clear()` |
| `periodic_task.hpp` | `PeriodicTask`: `start(name, stack, prio, core, period_us, fn, ctx, realtime=false)` calls `fn(ctx, elapsed_periods)` once per period; `setPeriod`, `stop`, `stackHighWaterMark` |
| `diag_sink.hpp` | `reportDiag(code, err, arg0, arg1)` and `countDiag(code, n)` |
| `kv_store.hpp` | `KvStore`: `get(ns, key, buf, len)` (a null `buf` gives the size), `set` (commits), `eraseNamespace`, `listNamespaces(fn, ctx)`; `nvsInit()` |
| `boot_info.hpp` | `resetWasCrash()`, `loadCrashCount()` / `storeCrashCount()` (RTC no-init), `reboot()` |
| `one_shot.hpp` | `OneShotTimer`: `start(name, delay_us, fn, ctx, realtime=false)` calls `fn(ctx)` once; `cancel()` |

Native only, for the tests:
- `core_os/native_diag.hpp`: what the diag sink recorded (`recordedReports()`, `reportCount()`, `counterTotal()`, `resetRecordedDiag()`).
- `core_os/native_kv.hpp`: fault injection for the in-memory `KvStore`. `failWritesAfter(n)` is a power loss (every write after the next `n` fails), `failWriteAfter(n, err)` / `failNextWrite(err)` fail one write, `tearWriteAfter(n, mode)` / `tearNextWrite(mode)` store a truncated or corrupted blob and still report success. Plus `clearFaults`, `writeCount`, `hasKey`, `removeKey`, `namespaceCount`. A write is a `set` or an `eraseNamespace`.
- `core_os/native_boot.hpp`: `setResetWasCrash()`, `rebootCount()`, `resetBootInfo()`. The crash counter is a process-wide variable, so it survives a `Core` being rebuilt, like RTC memory.

**IDF backend:**
- `PeriodicTask` is an `rtos_util::StoppableTask` woken by an `rtos_util::TaskTimer`, a periodic esp_timer with task dispatch. The elapsed count is the `ulTaskNotifyTake` count, so a late wake-up reports every period that went by. `stop()` destroys the timer (fenced) before stopping the task.
- `Mutex` and `Event` are static FreeRTOS semaphores (a mutex and a binary semaphore). `Event::wait` rounds the timeout up to whole ticks, plus one, because the timeout is a minimum.
- The diag sink calls `diag::report` / `diag::count`, and `reportDiag` also writes the code and args on the console (the core has no log text of its own).
- `KvStore` uses the `nvs` handle API on the default partition: it opens a handle per call (read-only for `get`, so a missing namespace isn't created) and commits every `set` and erase. `listNamespaces` walks the entries with `nvs_entry_find` and calls `fn` once per distinct namespace after the walk. `nvsInit()` runs `nvs_flash_init()`, and on `ESP_ERR_NVS_NO_FREE_PAGES` / `ESP_ERR_NVS_NEW_VERSION_FOUND` erases the partition and initialises it again (`erased` in the result; main reports `core.NvsInitFailed`). NVS has room for 254 namespaces per partition, and an erased namespace keeps its slot.
- The crash counter is three `RTC_NOINIT_ATTR` words (magic, count, complement): they survive every reset but a power-on, and read 0 when the magic or the complement is wrong. `resetWasCrash()` is `esp_reset_reason()` in {`ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`}; `reboot()` is `esp_restart()`.
- `OneShotTimer` is an esp_timer one-shot with task dispatch. `cancel()` stops it but doesn't wait for a callback that is already running.

**Native backend:**
- `PeriodicTask` is a `std::thread` that sleeps until each deadline (a condition variable, so `stop` and `setPeriod` wake it), with the same catch-up count.
- `Mutex` is a `std::mutex`, `Event` a mutex, condition variable and flag, and the clock `steady_clock`.
- The diag sink records calls for tests by default. `native::setDiagSinks` optionally installs the vdev's bounded record queue and fixed atomic counter table before starting tasks; detach only after all tasks are joined. No recording vector/map is used by the vdev's steady-state loop paths.
- `KvStore` is an in-memory map with fault injection by default. F3c adds `native_file.hpp`: `openFileStore(store, directory)` attaches a durable, exclusively-owned directory before boot. Every blob is flushed and atomically replaced separately, so the real core A/B algorithm still handles incomplete generations. `nvsInit()` does nothing. `openBootFile` persists the reset marker/crash counter across vdev processes; the normal native tests do not attach either file backend.
- `OneShotTimer` is a thread that waits on a condition variable; `cancel()` wakes and joins it.


## Rules

- **Allocate only in `start()`.** `PeriodicTask::start` creates the task (and the IDF timer), `OneShotTimer::start` its timer or thread. Nothing allocates after it. `KvStore` calls are configure-time operations (`listNamespaces` allocates).
- **NVS writes stall both cores' caches.** Never call `KvStore` from a real-time path; the core refuses SAVE while armed.
- **Real-time use:** a loop tick may call `Event::signal()`, `Mutex::tryLock()`, `nowUs()` and `countDiag()`. Nothing else here belongs on a real-time path. (The native `Event::signal` takes a mutex for a few instructions; it is a test backend.)
- `PeriodicTask` is not thread-safe: its owner serializes `start`, `setPeriod` and `stop`.
- `core_os` has no Kconfig of its own: the task parameters come from its callers (the core's `CoreConfig`).
- The IDF backend checks every IDF return and returns it as `Err`; it never aborts.

F3d: PeriodicTask subscribes real-time tasks to the ESP Task WDT before starting
the timer, resets the WDT after each callback (including paused/gated ticks),
and removes the subscription on cooperative stop. Setup waits at most one
second for the registration result. The native backend accepts the same flag.

`realtime.hpp` supplies RealtimeScope and the cumulative violation count. IDF
uses a no-op implementation. Native instruments C++ allocation and project C
malloc/calloc/realloc calls (GNU linker wrappers), Mutex::lock, Event::wait and
all KvStore operations. Native tests install a failure sink with
native_realtime.hpp; vdev leaves it unset and reports counter changes on stderr.
The default native diagnostic counters use fixed atomic storage, without a
mutex or an allocation in countDiag. Event::signal is still allowed in ticks.
`native_task.hpp` supplies one-shot task-start and timer-restart fault injection.
