# Component conventions

These rules apply to every component under `components/`. They add to the project-wide rules in the root `CLAUDE.md`: architecture, concurrency, real-time paths, protocol, failure handling and definition of done. Known violations are tracked in `review/10-docs-and-conventions.md`, and their fixes are in `TODO.md`.

## Layout

- Every component uses this layout. There are no exceptions for first-party code.
  - `include/`: public headers only.
  - `priv_include/`: internal headers only. A public header must never include a private one.
  - `src/`: implementation files.
  - `Kconfig`: tunable defaults and their valid ranges (see below).
  - `CMakeLists.txt`: `idf_component_register` with explicit `SRCS`, `INCLUDE_DIRS`, `PRIV_INCLUDE_DIRS` and `REQUIRES`.
  - `README.md`: the component's API, Kconfig options and, for protocol-facing components, the wire format.
- Third-party code lives untouched in a `vendor/` subfolder of the component that uses it. Wrap it; don't edit it.

## Kconfig scope

- Put in Kconfig anything a deployment might tune: task stack, priority and core; queue depths; periods and rates; timeouts; message IDs and stream channels; baud rates and pins. Give each option a `range`.
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
- Untrusted host input has a stricter rule: it must be rejected, not clamped. See "Protocol conventions" in the root `CLAUDE.md`.

## Resource management

- Allocate scarce peripherals (MCPWM, PCNT, ...) through a central pool or manager. Enforce its hard limits in the allocation path, and document them.
- Guard shared pool state with a FreeRTOS mutex. Create that mutex thread-safely, for example as a function-local static.
- Release everything deterministically in destructors, in reverse order of acquisition.

## Errors and logging

- Declare exactly one module-level tag per source file: `static const char *const TAG = "<module>";`. The `DIAG_*` macros log with it.
- Errors and warnings the host should know about go through `DIAG_ERROR` / `DIAG_WARN` (managers) or `DIAG_COUNT` (frequent faults, real-time paths), with a code from `components/diag/diag_codes.yaml`. Put the actionable detail (motor id, GPIO, failing stage) in the two args as well as in the text, because with `CONFIG_DIAG_CONSOLE_LOG=n` the text is gone.
- Hardware layers (`motor_hal`, `rotary_encoder_hal`, `rtos_util`, `pid`) don't depend on `diag`. They return `esp_err_t` / `bool`, and the manager that called them reports.
- Make logs actionable: include GPIOs, channels, operator or slot IDs, and the config values involved.
- Fail early, and leave the object in a consistent state after any failure. For example, release the DIR GPIO if MCPWM allocation fails afterwards.

## Naming and style

- **Files:** `snake_case.hpp` / `snake_case.cpp`, and every header starts with `#pragma once`.
- **Namespaces:** the namespace equals the component name (`uart_comm`, `motor_manager`). Internals go in a nested `detail` namespace.
- **Types** (classes, structs, `enum class`) and their enumerators use PascalCase. Use `enum class`, never plain `enum`.
- **Functions and methods:** camelCase (`isValid`, `setTiming`, `readU16Le`).
- **Variables and struct fields:** snake_case. Members are prefixed `m_`, file-scope statics `s_`.
- **Constants:** `constexpr` with a `k` prefix (`kProtocolVersion`, `kMaxCatchUpTicks`).
- **Macros and Kconfig symbols:** UPPER_SNAKE_CASE.
