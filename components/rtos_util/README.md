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
