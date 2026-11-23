# Component rules

These add to [firmware rules](../CLAUDE.md). Read the affected component's README
before changes; core subsystem contracts are in
[core/README.md](core/README.md) and [core_os/README.md](core_os/README.md).
New modules follow [the checklist](../../docs/adding-a-module.md).

## Boundaries

- Public headers go in `include/`, internal headers in `priv_include/`, code in
  `src/`; public headers must not include private ones. Explicitly list sources
  and IDF dependencies in CMake; public dependencies use `REQUIRES`.
- `core_os` backends use `backend/idf/` and `backend/native/`, selected by
  sources/include dirs, never `#ifdef`. Native-only filesystem helpers stay
  in the native backend.
- Core and pure module logic must build natively and may reuse platform-independent
  components. Keep hardware access, IDF/RTOS headers, `sdkconfig.h`, `CONFIG_*`
  and `esp_err_t` in the platform layer. Pass Kconfig settings to core through
  `CoreConfig` and use `core_os::Err` for cross-platform errors.
- Pure core reports through the `core_os` diagnostic sink. HALs, `rtos_util`
  and `pid` return errors without depending on `diag`; their caller reports.
- Deployment tunables belong in Kconfig with valid ranges; wire/math constants
  stay `constexpr`. Loop task settings are persisted params, not per-loop Kconfig.
  Third-party source stays untouched in `vendor/`; wrap it.

## Ownership and diagnostics

- Hardware owners are noncopyable; allow moves only when safe. Expose readiness
  and the initialization error when construction can fail. Stop/reset are
  explicit and idempotent. Caller synchronizes writes unless documented otherwise.
- Enforce hardware limits and prevent conflicting claims on scarce peripherals.
  Release acquired resources in reverse order, including after partial failures.
- Instance actions go through `Core::runAction`; paused actions require an
  acknowledged pause. Their fixed buffers must not escape the call.
- Diagnostic instance identity is the explicit instance argument, never a
  repurposed code arg. Keep actionable stage/cause/ID information in numeric
  args when console text is off; use named, documented numbers and never renumber.
  Module diag code 1 is `InitFailed`, reported instead of generic start failure.
- Resolve/claim interfaces before loops start; never copy shared ownership in a
  tick. Bind before acquiring hardware. A forced actuator owns its MCPWM operator;
  do not share a read-modify-write force register between outputs.

## Subsystem contracts

- Core README holds groups, sources, capture, testgen and paused-update contracts.
  Preserve owning-loop state and preallocated single-attempt claims; never
  spin/wait in loop/RX paths or read another loop's plain bindings.
- [mod_motor/README.md](mod_motor/README.md) holds interface lifetime, command
  freshness and emergency force. Keepalives never freshen stale setpoints.
- [mod_imu_bno055/README.md](mod_imu_bno055/README.md) holds instance-owned I2C,
  non-real-time placement and atomic paused calibration updates.
- [time.md](../../docs/protocol/time.md) and [ota.md](../../docs/protocol/ota.md)
  hold timestamp ownership and boot-selection/confirmation contracts.
