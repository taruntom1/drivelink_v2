# rtos_util

Small FreeRTOS helpers shared by the DriveLink components.

## `rtos_util::StoppableTask`

A task that is stopped by asking it to finish and then waiting for it. It replaces the old pattern of setting a flag, sleeping, then calling `vTaskDelete`.

```cpp
rtos_util::StoppableTask m_task;

m_task.start("worker", 4096, 5, [this] {
    TickType_t last_wake = xTaskGetTickCount();
    while (!m_task.stopRequested()) {
        doWork();
        m_task.sleepUntil(last_wake, pdMS_TO_TICKS(10));
    }
});

// In the owner's destructor, before freeing anything the body uses:
m_task.stop();   // wakes the body, waits until it returns, deletes the task
```

`start()` takes an optional last argument, `core_id`, to pin the task to a core (`xTaskCreatePinnedToCore`). The default is `tskNO_AFFINITY`.

How it works:
- When the body returns, the task gives a "done" semaphore and then suspends itself. It never deletes itself, so its handle stays valid until `stop()` deletes it.
- `stop()` sets the stop flag and sends a task notification, which wakes `sleepFor`, `sleepUntil` and any `ulTaskNotifyTake`. It then waits for "done".
- `stop()` never deletes a task whose body is still running, so a task can't be deleted while it holds a mutex. If the body takes a long time to return, `stop()` logs a warning every second while it waits.
- Inside the body, any wait that doesn't use a task notification (a queue receive, a driver call) must have a time limit. Otherwise the body never gets back to checking the stop flag.

## `rtos_util::TaskTimer`

A periodic `esp_timer` whose only job is to wake a `StoppableTask`. Use it for loops that must run at a fixed rate that does not depend on `CONFIG_FREERTOS_HZ` (the motor control loop, the IMU sampler).

```cpp
rtos_util::StoppableTask m_task;
rtos_util::TaskTimer m_timer;

m_task.start("sampler", 4096, 6, [this] {
    while (!m_task.stopRequested()) {
        // Count = timer periods elapsed since the last wake (+1 for a stop).
        const uint32_t fired = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        if (m_task.stopRequested()) break;
        if (fired == 0) continue;           // timeout: timer stopped or stalled
        if (fired > 1) overruns += fired - 1;
        doWork();
    }
});
m_timer.create(m_task, "sampler");
m_timer.start(10000);                       // every 10 ms
m_timer.setPeriod(5000);                    // live retime (esp_timer_restart)
m_timer.stop();                             // pause; start() resumes

// In the owner's destructor:
m_timer.destroy();   // stop, wait out an in-flight callback, delete
m_task.stop();
```

- The timer uses task dispatch (`ESP_TIMER_TASK`) and `skip_unhandled_events`. Its callback only calls `task.notify()`.
- `stop()` is idempotent and doesn't wait for a callback already in flight. That's harmless, because the callback only notifies.
- `destroy()` stops the timer, then waits for a one-shot "fence" timer on the esp_timer task. Callbacks on that task run one at a time, so once the fence has fired no callback of ours is still running. Call it before stopping the task.
- Calls aren't thread-safe; the owner serializes them (for example under its own mutex).

## `rtos_util::msToTicksCeil`

`tick_util.hpp`: converts milliseconds to ticks, rounding up. `pdMS_TO_TICKS` rounds down, so at the 100 Hz tick any value below 10 ms becomes 0 ticks, and a wait of 0 ticks doesn't wait. Use `msToTicksCeil` wherever the value is small or comes from configuration. Any `ms > 0` gives at least one tick.

Note that `vTaskDelay(n)` can return up to one tick early (it counts tick interrupts, not elapsed time). Where a delay is a minimum, as with sensor settling times, use `msToTicksCeil(ms) + 1`.
