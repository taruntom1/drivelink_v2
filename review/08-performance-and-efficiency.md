# 08: Performance and Efficiency

Nothing here is a crisis at the current load (≤ 4 motors, 100 Hz loop, a few kB/s of UART). These items matter as rates go up.

*Update:* the motor control loop now defaults to 1 kHz, driven by an esp_timer. Telemetry is still sent at 100 Hz by default, because it is decimated separately from the control rate.

---

## F1. Medium: the TX task scans all 64 stream slots and takes/gives a mutex per slot on every wake-up

[uart_comm.cpp:124-142](../components/uart_comm/src/uart_comm.cpp)

Every `sendCommand` and `sendData` wakes the TX task, which then does 64 `xSemaphoreTake/Give` pairs even when only one slot is dirty. At 100 Hz motor telemetry + 100 Hz IMU + command replies, that is ~13k mutex operations per second for 2–3 frames.

**Cheaper**: keep a `uint64_t dirty_mask` next to the slots. `sendData` sets the bit. The TX task swaps the mask out under one lock, then copies just those slots (one lock for all of them, or a per-slot spinlock). `__builtin_ctzll` iterates the set bits.

---

## F2. Medium: `RxFrame` (130 B) and `TxItem` (~140 B) are copied by value through queues and `std::function` callbacks

- `xQueueCreate(depth, sizeof(TxItem))`: each `sendCommand` encodes into a stack `TxItem`, and `xQueueSend` copies it again.
- RX: the frame is copied into the RX queue, or into MotorManager's config queue (130 B × 8) and IMU's (130 B × depth).
- [uart_comm.cpp:88-90](../components/uart_comm/src/uart_comm.cpp): **the `std::function` is copied out of the callback table under the mutex for every received frame**. Copying a `std::function` whose capture does not fit the small-buffer optimisation means a heap allocation per frame. `[this]` lambdas fit today, but nothing guarantees it.

**Cheaper**: make callback registration happen only before `start()` (or protect it with an RCU-style swap) and call the stored function by reference without locking. Or hold a lightweight function pointer plus `void*` context instead of `std::function`.

---

## F3. Low: `uart_comm::Impl` is large and allocated with plain `new`

`Impl` holds `std::array<StreamSlot,64>` (64 × 130 B ≈ 8.3 kB) plus `std::array<std::function,256>` (256 × 16 B = 4 kB). That is ~12.5 kB of internal RAM per instance, mostly unused (3 stream channels and ~6 callbacks are actually used). Options:
- Size the stream slot array with a Kconfig value, or store `payload` pointers allocated on first use.
- Use a small sorted vector for callbacks, or a 64-entry table indexed by `msg_type & 0x3F` per class.

---

## F4. Low: logging in hot paths

- [motor_controller.cpp:117,126](../components/motor_controller/src/motor_controller.cpp): `ESP_LOGD` with 3 floats per motor per 10 ms tick. It is compiled in at the default log level and evaluated whenever `LOG_LOCAL_LEVEL ≥ DEBUG`. Fine as debug, but don't enable it in production builds. **Fixed:** `update()` no longer logs.
- **Fixed:** neither the RX task nor the control tick logs setpoint problems any more. Bad frames and rejected entries are counted (`getSetpointStreamCounters()`), and the config task logs a summary at most once per second. Was: [motor_manager_uart.cpp:96-99](../components/motor_manager/src/motor_manager_uart.cpp): `ESP_LOGW` per rejected setpoint entry, on the RX task. A host streaming setpoints for a deleted motor at 100 Hz floods the log and slows RX. Rate-limit it. **Still open, and it now matters more:**
  - With the update task running, the RX task only fills the mailbox and rejects only on capacity.
  - An unknown motor id is rejected later, in `applyPendingSetpointsLocked()`. That runs on the control task while it holds `m_mutex`, and it logs one `ESP_LOGW` per entry.
  - If the console blocks, the control tick stalls and config commands time out.
  - Fix: rate-limit the warning, or reject unknown ids in `queueSetpoint()`.
- `PidController::setMode`/`reset` log at INFO, and `setMode` on the controller resets twice, so each mode change prints 3–4 lines.

---

## F5. Low: `getAngle()` / `getAngularVelocity()` re-read hardware in telemetry

[motor_controller.cpp:226-256](../components/motor_controller/src/motor_controller.cpp): in VelocityControl mode, `getAngle()` reads PCNT directly. In PositionControl mode, `getAngularVelocity()` calls `getRate()`, which may trigger a fresh PCNT read under a spinlock. `update()` has just read both values one line earlier in the same task tick. Cache both every update regardless of mode, and have the getters return the cache.

**Status: fixed.** `update(dt)` reads the encoder once per tick in every mode and caches angle, velocity and output. `getAngle()`, `getAngularVelocity()` and `getOutput()` return the cache, so telemetry never touches PCNT.

---

## F6. Low: `uart_write_bytes` is called per frame from the TX task

With `CONFIG_UART_COMM_TX_BUF_SIZE=1024`, each call copies into the driver ring buffer, which is fine. But when several dirty stream slots exist, encoding them all into one contiguous buffer and making one `uart_write_bytes` call halves the ring-buffer lock traffic. This only matters at much higher stream counts.
