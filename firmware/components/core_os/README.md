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
| `periodic_task.hpp` | `PeriodicTask`: `start(name, stack, prio, core, period_us, fn, ctx)` calls `fn(ctx, elapsed_periods)` once per period; `setPeriod`, `stop`, `stackHighWaterMark` |
| `diag_sink.hpp` | `reportDiag(code, err, arg0, arg1)` and `countDiag(code, n)` |

Native only: `core_os/native_diag.hpp` gives the tests what the diag sink recorded (`recordedReports()`, `reportCount()`, `counterTotal()`, `resetRecordedDiag()`).

**IDF backend:**
- `PeriodicTask` is an `rtos_util::StoppableTask` woken by an `rtos_util::TaskTimer`, a periodic esp_timer with task dispatch. The elapsed count is the `ulTaskNotifyTake` count, so a late wake-up reports every period that went by. `stop()` destroys the timer (fenced) before stopping the task.
- `Mutex` and `Event` are static FreeRTOS semaphores (a mutex and a binary semaphore). `Event::wait` rounds the timeout up to whole ticks, plus one, because the timeout is a minimum.
- The diag sink calls `diag::report` / `diag::count`, and `reportDiag` also writes the code and args on the console (the core has no log text of its own).

**Native backend:**
- `PeriodicTask` is a `std::thread` that sleeps until each deadline (a condition variable, so `stop` and `setPeriod` wake it), with the same catch-up count.
- `Mutex` is a `std::mutex`, `Event` a mutex, condition variable and flag, and the clock `steady_clock`.
- The diag sink records every call for the tests to inspect and reset.

F3a-2 adds the key-value store, the boot and reset info (an RTC counter) and reboot.

## Rules

- **Allocate only in `start()`.** `PeriodicTask::start` creates the task (and the IDF timer). Nothing allocates after it.
- **Real-time use:** a loop tick may call `Event::signal()`, `Mutex::tryLock()`, `nowUs()` and `countDiag()`. Nothing else here belongs on a real-time path. (The native `Event::signal` takes a mutex for a few instructions; it is a test backend.)
- `PeriodicTask` is not thread-safe: its owner serializes `start`, `setPeriod` and `stop`.
- `core_os` has no Kconfig of its own: the task parameters come from its callers (the core's `CoreConfig`).
- The IDF backend checks every IDF return and returns it as `Err`; it never aborts.
