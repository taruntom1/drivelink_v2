# 07: Code Quality, Reuse and Simplification

---

## Q1. Task start/stop logic is copy-pasted four times, each copy slightly different

`UartComm`, `MotorManager`, `TimeSync` and `ImuBno055Manager` each hand-roll "atomic stop flag + sleep + vTaskDelete". The four variants differ in grace time (20/150/200 ms), in whether they re-check the flag after a queue wake-up, and in whether they null the handle from inside the task. That is why each has its own bug (see [01](01-concurrency-and-lifecycle.md)).

**Simpler form**: one small shared helper (for example `components/rtos_util/include/stoppable_task.hpp`) with `start(fn, name, stack, prio)`, `requestStop()`, `join(timeout)`. Use task notifications for the wake-up and completion. Each manager then needs about five lines.

**Status: done** as `rtos_util::StoppableTask` (see C1 in [01](01-concurrency-and-lifecycle.md)).

---

## Q2. `motor_manager.cpp`: ten public methods repeat the same lock / find / unlock boilerplate

[motor_manager.cpp:173-509](../components/motor_manager/src/motor_manager.cpp): every method opens with `isValid` → `xSemaphoreTake(…)` → `findSlotIndexById` → manual `xSemaphoreGive` on each of 2–4 exit paths. That is about 150 lines of near-duplicates, and every extra early return is a chance to leak the lock.

**Simpler form**
```cpp
template <typename F>
ResultCode withMotor(uint8_t id, F&& fn) {
    if (!isValid()) return ResultCode::InvalidState;
    LockGuard lk(m_mutex, kMutexTimeoutTicks);   // tiny RAII wrapper
    if (!lk) return ResultCode::InternalError;
    auto* slot = findSlot(id);
    if (!slot) return ResultCode::NotFound;
    return fn(*slot);
}
```
A RAII `LockGuard` for FreeRTOS mutexes would also replace the manual give/take pairs in `ImuBno055Manager`, `Bno055Device` (8 `unlock(); return ESP_FAIL;` sites in `readSample` alone) and `TimeSync`.

The repeated `if (m_use_mock_feedback || motor == nullptr)` branches (8 occurrences) would also go away if mock mode used a `MockMotorController` implementing the same interface, instead of being special-cased everywhere.

---

## Q3. Duplicated little-endian helpers

**Fixed.** All three components now use `uart_comm::wire::Reader` / `Writer` from `uart_comm/include/wire_codec.hpp`, which has a sticky error flag.

Byte-order helpers are implemented separately in:
- [motor_manager_uart.cpp:23-41](../components/motor_manager/src/motor_manager_uart.cpp): `readFloatLe`, `readU32Le`
- [time_sync_protocol.cpp:3-33](../components/time_sync/src/time_sync_protocol.cpp): `writeU16LE`, `writeI64LE`, `readU16LE`, `readI64LE`
- [imu_bmo_055.cpp:32-66](../components/imu_bmo_055/src/imu_bmo_055.cpp): `appendU16Le`, `appendU64Le`, `appendS16Le`, `readU16Le`
- `encodeTelemetryLocked` uses raw `memcpy` for the same job.

They also differ in style (bounds-checked or not; the `append*` helpers silently drop data on overflow and leave `offset` unchanged, so later writes land in the wrong place). Put one `ByteWriter` / `ByteReader` with a sticky error flag in `uart_comm` (it is already a dependency of every protocol component) and reuse it.

---

## Q4. A symptom-level special case in the I²C write path

[bno055_device.cpp:439-460](../components/imu_bmo_055/src/bno055_device.cpp), from the most recent commit `ded0a7d`:

```cpp
const bool is_page_id_write = (reg_addr == BNO055_PAGE_ID_REG && cnt == 1U);
const unsigned attempts = is_page_id_write ? 3U : 1U;
const uint32_t timeout_ms = is_page_id_write ? 500U : m_config.timeout_ms;
```

This patches the first register write that happens to fail. The underlying problem is that the sensor isn't ready yet: the constructor waits only 100 ms after power-up, while the datasheet POR-to-config time is about 650 ms. It may also be clock stretching, which the BNO055 is known for. Either way the fix belongs in initialisation, not in the shared bus-write path:

1. After `i2c_driver_install`, poll `CHIP_ID` (reg 0x00) until it reads `0xA0`, with a ~1 s timeout.
2. If clock stretching is the issue, raise the I²C timeout globally (`i2c_set_timeout`).
3. If retries are still wanted, apply them uniformly to all transactions, behind a Kconfig value.

---

## Q5. Dead or unused code

| Item | Location |
|------|----------|
| `bno055_support.c` (Bosch template), now being added to the build | **Fixed:** deleted (I3 in [04](04-imu-bno055.md)). |
| `IMU_BMO_055_EVENT_MSG_TYPE` Kconfig: defined, never used | [imu Kconfig](../components/imu_bmo_055/Kconfig) |
| `time_sync::protocol::decodeRequest` / `encodeResponse`: server-side helpers, unused on the device | [time_sync_protocol.cpp](../components/time_sync/src/time_sync_protocol.cpp) (fine for tests, but they are not tested) |
| `PidController` copy constructor/assignment: deep copy of an object that is only ever a member, never copied | [pid_controller.cpp:105-129](../components/pid/src/pid_controller.cpp) |
| `ImuStreamSample` carries every field even when the mask selects one | minor |
| `uart_comm.cpp` constructor zeroes `callbacks[]` and other fields that `Impl{}` value-initialises already. **Done** with the C1–C3 fix. | [uart_comm.cpp](../components/uart_comm/src/uart_comm.cpp) |
| `Parser::handlePatternDetected(RxFrame&, bool& success)`: out-param `bool` instead of a return value. **No longer applicable:** the parser rewrite (P1) removed it; `nextFrame()` returns `bool`. | [parser.cpp:65](../components/uart_comm/src/parser.cpp) |

---

## Q6. `pimpl` via raw `new` / `delete` with NOSONAR comments

`UartComm`, `Encoder`, `Motor`, `PidController` and `TimeSync` manage `Impl*` by hand, with custom move operations. `std::unique_ptr<Impl>` (with the destructor defined in the .cpp) gives the same ABI isolation, removes the hand-written move constructor/assignment, and removes the NOSONAR suppressions. Teardown order inside `~X()` can stay explicit.

---

## Q7. Naming

The component and namespace are called `imu_bmo_055` / `ImuBno055Manager`, but the part is the **BNO055** (`bmo` is a typo). This affects the directory, namespace, Kconfig symbols and headers. Renaming is mechanical and is cheapest done before external users depend on it.
