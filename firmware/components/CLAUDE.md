# Component conventions

These rules apply to every component under `firmware/components/`. They add to the firmware-wide rules in `firmware/CLAUDE.md`: architecture, concurrency, real-time paths, protocol, failure handling and definition of done.

## Layout

- Every component uses this layout. There are no exceptions for first-party code.
  - `include/`: public headers only.
  - `priv_include/`: internal headers only. A public header must never include a private one.
  - `src/`: implementation files.
  - `Kconfig`: tunable defaults and their valid ranges (see below).
  - `CMakeLists.txt`: `idf_component_register` with explicit `SRCS`, `INCLUDE_DIRS`, `PRIV_INCLUDE_DIRS` and `REQUIRES`. List every IDF component whose headers the component includes: the firmware builds only what `main` requires (`firmware/CMakeLists.txt`), so nothing else is there to find.
  - `README.md`: the component's purpose, API, Kconfig options and rules. The wire format lives in `docs/protocol/`, not here. During the overhaul, legacy components have no README, and new components add one when they land.
- Third-party code lives untouched in a `vendor/` subfolder of the component that uses it. Wrap it; don't edit it.
- `core_os` is the one exception to `src/`: its sources are in `backend/idf/src` and `backend/native/src`, and each backend has its own `include` dir for `core_os/detail/backend_types.hpp`. The build picks the backend by include dir and sources, never by `#ifdef`.
- Native-only vdev helpers may select OS filesystem primitives inside the native backend (`native_file.cpp`). File storage is configure-time only. The default unit-test store remains in-memory. Optional diagnostic sinks are installed before starting tasks and detached after joining them; the vdev's loop counters use fixed atomic storage.

## The pure core

- `core` (and, later, the pure parts of the module components) uses only `core_os`, `protocol/` and the C++ standard library. No ESP-IDF or FreeRTOS header, no `sdkconfig.h`, no `CONFIG_` symbol and no `esp_err_t`, so it builds natively (`firmware/test/native`, later the virtual device).
- Kconfig reaches it through `CoreConfig`, which only `firmware/components/core/src/idf/core_config_idf.cpp` fills. Native builds fill it directly.
- It has no logging. It reports through the `core_os` diag sink (`reportDiag`, `countDiag`), which calls diag on the device and records the calls natively.
- Errors cross `core_os` as `core_os::Err` (an `esp_err_t` value on the device, 0 for success).

## Kconfig scope

- Put in Kconfig anything a deployment might tune: task stack, priority and core; queue depths; periods and rates; timeouts; baud rates and pins. Give each option a `range`.
- MSG_TYPEs and stream channels are not Kconfig items. They come from the generated registry (`protocol/defs/registry.yaml`, header `drivelink/protocol/gen/registry.hpp`). A component whose public header includes that header lists `protocol` in `REQUIRES`, not `PRIV_REQUIRES`.
- Keep pure implementation constants as `constexpr` in a header or `.cpp`. These are wire-format field sizes, limits tied to the protocol, and math constants.

## Public API

- Keep the API small and focused on the hardware or job.
- Hide low-level driver state in an `Impl`, a pool or a `detail` struct.
- Hardware-owning types delete copy construction and copy assignment. They allow move only when transferring ownership is safe.
- Provide an `isValid()` readiness check whenever construction can fail.
- Convenience setters, such as percent-based ones, go through one canonical implementation path.
- Stop and reset operations are explicit and idempotent.
- Concurrent writes to the same instance must be synchronized by the caller, unless the type is documented as thread-safe.

## Numeric inputs at the C++ API boundary

- Clamp out-of-range numbers to the documented range.
- Reject values that cannot be clamped meaningfully, and return an error:
  - non-finite floats
  - invalid enum values
  - GPIOs outside the allowed set
- Untrusted host input has a stricter rule: it must be rejected, not clamped. See "Protocol conventions" in `firmware/CLAUDE.md`.

## Resource management

- Allocate scarce peripherals (MCPWM, PCNT, ...) through a central pool or manager. Enforce its hard limits in the allocation path, and document them.
- Guard shared pool state with a FreeRTOS mutex. Create that mutex thread-safely, for example as a function-local static.
- Release everything deterministically in destructors, in reverse order of acquisition.

## Errors and logging

- Declare exactly one module-level tag per source file: `static const char *const TAG = "<module>";`. The `DIAG_*` macros log with it.
- Errors and warnings the host should know about go through `DIAG_ERROR` / `DIAG_WARN` (managers) or `DIAG_COUNT` (frequent events) / `DIAG_RAISE` (latched conditions, real-time paths), with a code from `protocol/defs/diag_codes.yaml`. Put the actionable detail (motor id, GPIO, failing stage) in the two args as well as in the text, because with `CONFIG_DIAG_CONSOLE_LOG=n` the text is gone.
- Hardware layers (`motor_hal`, `rotary_encoder_hal`, `rtos_util`, `pid`) don't depend on `diag`. They return `esp_err_t` / `bool`, and the manager that called them reports. A type whose construction can fail keeps the reason for its caller (`initError()`), so the report carries the real `esp_err` and stage. The pure `core` doesn't depend on `diag` either: it reports through the `core_os` diag sink.
- Stage and cause numbers passed as diag args are part of the wire contract: define them as named constants next to the report, document them in `protocol/defs/diag_codes.yaml`, and never renumber them.
- Make logs actionable: include GPIOs, channels, operator or slot IDs, and the config values involved.
- Fail early, and leave the object in a consistent state after any failure. For example, release the DIR GPIO if MCPWM allocation fails afterwards.

## Naming and style

- **Files:** `snake_case.hpp` / `snake_case.cpp`, and every header starts with `#pragma once`.
- **Namespaces:** the namespace equals the component name (`uart_comm`, `mod_motor`). Internals go in a nested `detail` namespace.
- **Types** (classes, structs, `enum class`) and their enumerators use PascalCase. Use `enum class`, never plain `enum`.
- **Functions and methods:** camelCase (`isValid`, `setTiming`, `readU16Le`).
- **Variables and struct fields:** snake_case. Members are prefixed `m_`, file-scope statics `s_`.
- **Constants:** `constexpr` with a `k` prefix (`kProtocolVersion`, `kMaxCatchUpTicks`).
- **Macros and Kconfig symbols:** UPPER_SNAKE_CASE.

## F3b actions and diagnostics

Instance actions run through Core::runAction. Loop actions follow parameter
application; paused actions require an acknowledged pause. Their fixed storage
must not escape the call. Diagnostic instance identities are explicit; use
core_os::reportDiag's instance argument for instance reports. Do not encode
instance identity by overwriting existing code args.

F3d loop tasks use persisted loop parameters, not per-loop Kconfig task options.
Only realtime loops join the Task WDT. Native real-time tests fail on allocation
or blocking core_os calls during ticks; fixture observations must be preallocated.

F4a group access transfers slot ownership with one atomic claim and never
waits or spins in a loop/RX path. Configuration shadows belong only to the
config caller; signals and inputs are accessed only by the owning loop.

F4c source links use SPSC triple buffers, one per ordered loop pair. Resolve
and allocate before enabling loops. Never read another loop's plain bindings
from a consumer tick; only the producer latches those bindings. Sampled
storage belongs to the consumer loop. ARM/DISARM do not reset it.

F5a-1 motor interfaces use startup-only `Instance::queryInterface` and a shared
control-port lifetime. Resolve/claim before loops start; never copy shared
ownership in a tick. Commands, observations and control-port fields belong to
the same loop. Command input receipt timestamps are copied only with validated
post-ARM value frames; keepalives never make a mode's stale setpoint fresh.

F5a-2 hardware modules keep their logic pure and reach drivers only through
`mod_motor/hardware.hpp` (device backend in `src/idf/`, a fake in the native
tests). Bind first and take hardware last in `start()`. An output that the
monitor forces must not share a read-modify-write force register with another
output (one MCPWM operator per actuator). A type's diag code 1 is its
`InitFailed`, which the core reports in place of `core.InstanceStartFailed`.

F6 adds a non-real-time bus seam in `mod_imu_bno055`. Only that module
acquires I2C; its type forbids real-time loops. Steady-state reads allocate
nothing. A successful paused action may request up to 24 internal parameter
updates through `ActionOut`; the core validates the entire batch, checks the
owning acknowledged pause, updates hookless live bindings and shadows, and
marks persistence dirty before resuming the loop. There is no wire metadata
change and no direct module access to the config shadow.

F7a capture records on its loop under a single-attempt claim and never waits;
the config caller writes the capture's configuration and ring only while the
loop leaves them alone (Idle or Done) and publishes them with the Armed
release store. The ring is the only PSRAM allocation; allocate it only at
capture CONFIG through `core_os::captureAlloc`. Never put an atomic or other
shared state in PSRAM (IDF serves atomics there with a critical section);
assert lock-freedom with `core_os::lockFreeAtomic<T>()`.

F7b testgen writes a generated input and its actual write-time receipt only
on its owning loop, after command consumption and before instances tick.
Each input binding retains a testgen-active gate and termination timestamp;
command groups must suppress both the value and receipt for that target while
active and reject mailboxes received at or before termination. Other entries
and the frame's watchdog receipt are unaffected. ARM/DISARM reset the gates.
Per-loop requests use Ready/Applying/Applied staging; terminal snapshots are
immutable behind a release/acquire pending flag, published before live Done
so restart cannot overwrite a completion. Only 32-bit-or-smaller atomics are
used, in internal RAM. See the core README for ownership and snapshot rules.

F8 time pings bypass the config queue and never block/retry in RX. A queued
time reply owns its bytes and stamp flag; only TX patches it. Device time is
monotonic since boot, never host-set. See `docs/protocol/time.md`.
