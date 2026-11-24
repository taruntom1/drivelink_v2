# rtos_util

Small FreeRTOS helpers for firmware tasks: a stoppable task, a timer that wakes it, and tick rounding. The rules for using them are in [firmware/CLAUDE.md](../../CLAUDE.md).

## StoppableTask ([`stoppable_task.hpp`](include/stoppable_task.hpp))

Owns one task with a cooperative stop and a real join, so a task is never deleted mid-body (for example while holding a mutex) and its handle is never used after it is gone. Not copyable or movable; the object must outlive the task, and its destructor calls `stop()`.

| Call | Contract |
|---|---|
| `start(name, stack, priority, body[, core_id])` | Creates the task, optionally pinned to `core_id`. Returns `ESP_ERR_INVALID_STATE` if already running, `ESP_ERR_INVALID_ARG` for an empty body or invalid core, `ESP_ERR_NO_MEM` if creation fails. |
| `requestStop()` | Sets the stop flag and wakes the body if it sleeps in a helper or `ulTaskNotifyTake()`. Does not wait; safe when not running. |
| `stop()` | `requestStop()`, then waits until the body returns and deletes the task. Idempotent. Waits as long as it takes, logging a warning every second, so every blocking wait in the body must be bounded. Never call it from the task itself. |
| `stopRequested()`, `isRunning()` | The body polls `stopRequested()` and returns once it is true. `isRunning()` is true from a successful `start()` to the end of `stop()`. |
| `sleepFor(ticks)`, `sleepUntil(last_wake, period)` | Body only. Return false early on a stop request. `sleepUntil` has `vTaskDelayUntil` semantics and treats a period of 0 as one tick, so it cannot spin. |
| `notify()`, `stackHighWaterMark()` | Wake the task (`xTaskNotifyGive`; no-op when not running) and report its minimum free stack in bytes (0 when not running). Do not call either concurrently with `stop()`. |

## TaskTimer ([`task_timer.hpp`](include/task_timer.hpp))

A periodic `esp_timer` with task dispatch and `skip_unhandled_events` whose callback only notifies a `StoppableTask`. The woken task's `ulTaskNotifyTake(pdTRUE, timeout)` count is the number of periods elapsed since it last woke, plus one for a stop request. Not copyable or movable; the destructor calls `destroy()`.

| Call | Contract |
|---|---|
| `create(task, name)` | Creates the stopped timer; `task` and `name` must outlive it. `ESP_ERR_INVALID_STATE` if already created. |
| `start(period_us)`, `setPeriod(period_us)` | `start` fires every `period_us`, restarting with the new period if running; 0 is `ESP_ERR_INVALID_ARG`. `setPeriod` restarts a running timer or stores the period for the next `start`; on error the old period stays. |
| `stop()` | Stops firing. Idempotent; does not wait for an in-flight callback, which only notifies. |
| `destroy()` | Stops, waits out an in-flight callback, then deletes the timer, so nothing touches the task afterwards. Idempotent, also when `create()` never ran or failed. |

Calls may come in any order between `create()` and `destroy()`. The owner serializes them; the callback is safe alongside any of them.

## Ticks ([`tick_util.hpp`](include/tick_util.hpp))

`msToTicksCeil(ms)` rounds up, so any `ms > 0` waits at least one tick. `pdMS_TO_TICKS` rounds down, and with the 10 ms tick any value below 10 ms becomes no wait at all.
