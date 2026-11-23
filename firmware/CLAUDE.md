# Firmware rules

Paths in backticks are repo-relative. The firmware uses ESP-IDF 5.5.2.

## Commands

Build/test commands, ESP-IDF setup, build traps and hardware commands are in
[docs/development.md](../docs/development.md).

## Real-time and lifecycle constraints

- Allocate only during setup or configuration. Keep steady-state paths allocation-free.
- Real-time paths include realtime loop ticks, staging swaps/OnChange hooks,
  timer/driver callbacks, ISRs, critical sections and UART RX callbacks. No heap,
  blocking or host-dependent loop bounds; only zero-timeout try-locks/queues.
  Work under `m_mutex` stays short and fixed-size.
  No logging in ISRs, critical sections, spinlocks or under `m_mutex`; rate-limit
  other real-time logs and prefer diagnostic counters.
- Use `rtos_util::StoppableTask`, never bare task create/delete. Bound waits;
  stop/join before freeing task state. Use `TaskTimer` for fixed-rate loops;
  serialize timer operations and fence `destroy()` before stopping the task.
- FreeRTOS ticks are 10 ms. Use `msToTicksCeil` for small/configurable delays;
  add one tick where a delay must be a minimum (sensor settling).
- UART registrations are RAII: reset them first in destructors, before state or
  `UartComm` dies. Callbacks hand off work without blocking. Open stream slots
  at setup; `sendData` stays nonblocking and allocation-free.
- Every task must retain more than 1 KB free; add new non-loop tasks to main's
  stack-log list. An `RxFrame`/maximum payload is about 1 KB; check stack marks
  before putting either on a task stack and account for queue storage.
- Loops never wait for config. The staging/shadow ownership contract is in
  [core/README.md](components/core/README.md).

## Memory, failures and persistence

- PSRAM stays caps-alloc only, never `SPIRAM_USE_MALLOC`; only capture's ring
  uses it, at CONFIG via `core_os::captureAlloc`. Keep atomics/shared state in
  internal RAM; use `core_os::lockFreeAtomic<T>()`, not `is_always_lock_free`.
  XiP remains off in normal firmware.
- Exceptions and RTTI are disabled. Setup allocation failure is fatal, so
  `new` and `make_unique` are acceptable during setup.
- Check IDF results and report failures with `esp_err_to_name`. No
  `ESP_ERROR_CHECK`; host/hardware runtime failures return errors, never
  assert/abort. Assertions are only for programmer invariants.
- Host-visible errors use `DIAG_ERROR`/`DIAG_WARN`; frequent events use
  `DIAG_COUNT`, latched conditions use `DIAG_RAISE`/`DIAG_CLEAR`. Report condition
  transitions once and clear on recovery/shutdown; real-time tasks count and
  non-real-time tasks watch/report. `ESP_LOGx` is for info, programmer errors or
  already-reported command refusals. Keep the no-console build warning-free.
- NVS belongs to `core_os::KvStore`; flash writes stall both cores. Keep them
  off real-time paths; SAVE/FACTORY_RESET are refused while armed.
- OTA/partition rules are in [ota.md](../docs/protocol/ota.md). Preserve NVS,
  dual slots and rollback; never erase flash for an update or confirm at startup.
  Flash work belongs to config, never tick/RX/monitor code.

## Wire and verification

- Use shared Reader/Writer field by field, never struct copies/local endian
  helpers. Reject invalid host input without changes. Follow each operation's
  contract in `docs/protocol/` for validation, clamping and reporting.
- Give each rejection its own result/detail; detail IDs never renumber. Commands
  get responses even on queue-full refusal. Time pings are the malformed-drop/count
  exception; see [time.md](../docs/protocol/time.md). Message/channel IDs come
  from the generated registry, never literals or Kconfig.
- Don't hand-edit `sdkconfig` or build outputs. Use committed config fragments;
  commands and restoration traps are in [the development guide](../docs/development.md).
- New pure sources also join `firmware/test/native/CMakeLists.txt`; see
  [test rules](test/CLAUDE.md).
- Published firmware ships `notices/THIRD_PARTY_NOTICES.txt`: a newly linked
  third-party library or IDF component under its own licence needs a
  `notices/notices.json` entry (see [README.md](README.md)).
