# rtos_util

Small FreeRTOS helpers that every firmware task uses. No Kconfig options.

## API

| Header | Type | Use |
|---|---|---|
| `stoppable_task.hpp` | `StoppableTask` | Owns one task. `start(name, stack, priority, body, ...)`; the body loops on `stopRequested()` and sleeps with `sleepFor` / `sleepUntil`, which return early on a stop request. `stop()` requests the stop and waits for the body to return; the destructor calls it. `notify()` wakes the task; `stackHighWaterMark()` reports its free stack. |
| `task_timer.hpp` | `TaskTimer` | A periodic `esp_timer` (task dispatch, unhandled events skipped) whose callback only notifies a `StoppableTask`. The task's `ulTaskNotifyTake(pdTRUE, timeout)` count is the number of elapsed periods. Lifecycle: `create()`, then `start` / `setPeriod` / `stop` in any order, then `destroy()`, which waits out a running callback. |
| `tick_util.hpp` | `msToTicksCeil(ms)` | Milliseconds to ticks, rounded up: any `ms > 0` is at least one tick (the tick is 10 ms). |

## Rules

- Create every task with `StoppableTask`; never bare `xTaskCreate` /
  `vTaskDelete`. Every other blocking wait has a bounded timeout.
- The owner of a `TaskTimer` serializes its calls and calls `destroy()` before
  stopping the task the timer wakes.
- Don't use `pdMS_TO_TICKS` for small or configurable values; it rounds down
  to zero below one tick.
