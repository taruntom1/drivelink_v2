# 01: Concurrency and Task Lifecycle

Every manager class (`UartComm`, `MotorManager`, `TimeSync`, `ImuBno055Manager`) owns FreeRTOS tasks. All of them stop those tasks the same way: set an atomic stop flag, `vTaskDelay(grace)`, then `vTaskDelete(handle)`. That pattern is unsafe for two opposite reasons, depending on timing.

---

## C1. High: `vTaskDelete()` on a task handle that already deleted itself

**Where**
- [motor_manager.cpp:663-674](../components/motor_manager/src/motor_manager.cpp): `stopUpdateTask()`
- [motor_manager.cpp:139-148](../components/motor_manager/src/motor_manager.cpp): config task in `~MotorManager`
- [imu_bmo_055.cpp:712-735](../components/imu_bmo_055/src/imu_bmo_055.cpp): `stopTasks()`
- [imu_bmo_055.cpp:377-384](../components/imu_bmo_055/src/imu_bmo_055.cpp): constructor failure path

**What happens**
Each task loop ends with `vTaskDelete(nullptr)` once it sees the stop flag. The owner then waits 20 ms and calls `vTaskDelete(m_xxx_task)` on the saved handle without checking whether the task is still alive.

- The motor update task runs every 10 ms, so it almost always sees the flag and deletes itself inside the 20 ms grace window.
- The config tasks are woken on purpose with a dummy queue item, so they exit almost immediately.

When the owner then deletes the stale handle, it acts on a TCB that is already queued for cleanup by the idle task, or already freed. The result is an assert in `tasks.c`, heap corruption, or deleting an unrelated task that reused the memory.

**When it triggers**
Any time a manager is destroyed. In `app_main` that happens on every early `return`. Example: `time_sync_obj.isValid()` is false, or the IMU's `setOperationMode` fails. The firmware then crashes while unwinding instead of just idling.

**Fix**
Use a real join. Each task signals completion (`xTaskNotifyGive(owner)` or a binary semaphore / event-group bit) just before `vTaskDelete(nullptr)`. The owner waits on that signal with a timeout and never calls `vTaskDelete` on a handle the task may already have released. Put this in one shared helper (for example `rtos::StoppableTask`) so the four components stop re-implementing it.

**Status: fixed.** A new `components/rtos_util` component provides `StoppableTask`. When a task's body returns, the task gives a "done" semaphore and suspends itself; it never deletes itself. The owner's `stop()` sets the stop flag, sends a task notification to wake the body, waits for "done" (logging a warning every second), and only then deletes the task. `MotorManager`, `ImuBno055Manager`, `TimeSync` and `UartComm` all use it. Periodic sleeps now use `sleepFor` / `sleepUntil`, so a stop request wakes them straight away instead of waiting out a 5 s TimeSync interval or an IMU stream period.

---

## C2. High: forced `vTaskDelete()` while the task holds a mutex

**Where**: the same call sites, plus [time_sync.cpp:244-255](../components/time_sync/time_sync.cpp).

When the grace period is too short, the task is still running and gets force-deleted:
- The IMU stream task blocks in `vTaskDelayUntil(period)` for up to `max_stream_period_ms`, or sits in an I²C transaction while holding `Bno055Device::m_mutex` and the I²C driver lock.
- TimeSync's periodic task sleeps in `vTaskDelayUntil(5000 ms)`. Its 200 ms poll loop therefore **always** times out and force-deletes it, sometimes in the middle of `performSync()` while holding `sync_mutex`.

FreeRTOS does not release mutexes held by a deleted task. Any later `xSemaphoreTake(..., portMAX_DELAY)` on that mutex deadlocks. One example is `~MotorManager` taking `m_mutex` at [motor_manager.cpp:151](../components/motor_manager/src/motor_manager.cpp) after the update task was killed inside `updateAll()`. An I²C transaction cut off halfway can also leave the bus stuck.

**Fix**: same as C1. Wake sleeping tasks with `xTaskAbortDelay()` or a notification instead of waiting out their delay, then join.

**Status: fixed** together with C1. `xTaskAbortDelay()` was not used: it would also abort blocking calls that ignore their return value, such as `xSemaphoreTake(..., portMAX_DELAY)`, so a task notification is used instead.

---

## C3. Medium: `UartComm` destructor "joins" with a fixed 200 ms sleep

[uart_comm.cpp:312-338](../components/uart_comm/src/uart_comm.cpp)

The RX task can be inside a user callback, or blocked for 100 ms in `xQueueReceive` and then up to 2 × 100 ms in `uart_read_bytes` in the parser. The destructor sleeps 200 ms, then deletes the UART driver, the queues, the mutexes and `parser`. If the RX task is still running, it touches freed memory.

Also, callbacks registered by other managers capture `this`. Because `UartComm` is constructed first in `app_main`, it is destroyed last, so that order is fine. But nothing in the API enforces it.

**Fix**: real join (see C1). Document that callbacks must be unregistered before their owner dies. Better, have `registerCallback` return a RAII token.

**Status: fixed.**
- `~UartComm` now joins both tasks through `StoppableTask` before it frees the driver, queues, mutexes and parser.
- `registerCallback(msg_type, cb, registration)` fills in a move-only `uart_comm::CallbackRegistration` that unregisters the callback when destroyed or `reset()`.
- The RX task now holds a recursive `callback_mutex` while it runs a callback. Unregistering therefore waits for a callback that is already running, which closes the window where a manager could be destroyed mid-callback.
- Registering a second callback for the same `msg_type` is rejected.
- `~UartComm` aborts with a log message if any registration is still active.
- All managers now reset their registrations first in their destructors.

---

## C4. Medium: RX callbacks run on the RX task, and the setpoint callback blocks on the manager mutex

[uart_comm.cpp:87-99](../components/uart_comm/src/uart_comm.cpp), [motor_manager_uart.cpp:62-101](../components/motor_manager/src/motor_manager_uart.cpp)

`handleSetpointStream` calls `setSetpoint()` once per entry. Each call takes `m_mutex` with a 50 ms timeout. While the update task holds the mutex for `updateAll()`, and the config task holds it for `createMotor()` (which creates MCPWM/PCNT resources and logs), the RX task is blocked and cannot drain the UART event queue. Combined with finding P1 in [02](02-uart-protocol-and-framing.md), frames are lost.

Configure commands are already handed off to a queue. The setpoint stream should be too: copy it into a latest-wins buffer under a short critical section, and apply it in the update task.

**Status: fixed.** `handleSetpointStream` now writes each entry into a per-motor mailbox (`m_pending_setpoints`, one latest value per setpoint kind, sized to `max_motors`) under a `portMUX` spinlock, and never takes `m_mutex`. The update task drains the mailbox at the start of each tick (`applyPendingSetpoints`). Setpoints take effect within one update period. If the update task is disabled, the callback applies setpoints directly, as before.

*Update (timer-driven control loop):* the drain is now `applyPendingSetpointsLocked()`. It runs inside the control tick's single try-lock of `m_mutex`, so the tick never waits on the mutex. If the mutex is busy, the setpoints stay in the mailbox until the next tick.

---

## C5. Low: unsynchronised shared fields

- `TimeSync::Impl::periodic_task` is written by the task ([time_sync.cpp:335](../components/time_sync/time_sync.cpp)) and polled by the owner ([time_sync.cpp:245](../components/time_sync/time_sync.cpp)) as a plain pointer. It should be atomic, or replaced with a join signal.
- `TimeSync::triggerManualSync(false, …)` increments `next_request_id` outside `sync_mutex` ([time_sync.cpp:187](../components/time_sync/time_sync.cpp)), racing with `performSync()`.
- `Parser::m_counters` is written by the RX task and read by `getErrorCounters()` from any task. 32-bit reads are atomic on Xtensa, so this is benign today, but the struct copy is not atomic as a whole.
- `PcntPool::ensureInitialized()` / `McpwmPool::ensureInitialized()` lazily create the pool mutex with no protection ([pcnt_pool.cpp:15](../components/rotary_encoder_hal/src/pcnt_pool.cpp), [mcpwm_pool.cpp:15](../components/motor_hal/src/mcpwm_pool.cpp)). Two tasks constructing the first motor or encoder at the same time can each create a mutex, and one leaks. Use a function-local static or create the mutex in a `static` initializer.

---

**Status: fixed.**
- **`periodic_task`:** replaced by `StoppableTask` (see C1).
- **`next_request_id`:** now `std::atomic<uint16_t>` with `fetch_add`.
- **Parser counters:** each one is `std::atomic<uint32_t>`.
- **MCPWM/PCNT pool mutexes:** created in a function-local static (`poolMutex()`), whose initialisation C++ guarantees happens once.

---

## C6. Low: `ImuBno055Manager` config task dispatches the shutdown wake-up frame

[imu_bmo_055.cpp:783-807](../components/imu_bmo_055/src/imu_bmo_055.cpp)

Unlike `MotorManager::configTaskEntry`, the IMU config task does not re-check the stop flag after `xQueueReceive`. The zeroed wake frame is parsed (`len < 2` gives `InvalidArgument`) and a configure **response is transmitted to the host during shutdown**. Add the same `if (stop) break;` check that motor_manager has.

**Status: fixed.** The config task now re-checks `stopRequested()` after `xQueueReceive` and exits without dispatching or replying.
