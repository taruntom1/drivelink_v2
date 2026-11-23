# Test rules

These add to [firmware rules](../CLAUDE.md). Paths below are repo-relative.
Commands/toolchains, check selection, logs and CI behavior are in
[docs/development.md](../../docs/development.md).

## Native tests

- Register new native-build sources alongside the related implementation in
  `firmware/test/native/CMakeLists.txt`. Protocol tests live in
  `protocol/test/`. Core must not depend on shim headers. For non-core
  sources needing a new `CONFIG_*`, update the native shim defaults.
- Keep native KV storage in-memory by default. Model reboot with a new Core
  on the same store. Use an isolated file store when testing file persistence.
  Reuse generator fixture types
  and the existing layout/boot helpers.
- Threaded timing bounds must tolerate desktop scheduler delays (about 15 ms).
  Preallocate real-time fixture observations. The native guard fails unexpected
  allocation/blocking; suppress its sink only for deliberate violation tests.
  Capture concurrency tests sample directly: a scheduler-delayed whole `runTick`
  can count an overrun and allocate through the native diagnostic sink.
- Test helpers may boundedly retry `Withdrawn` and group-only `StagingBusy`,
  which guarantee no effect. Never extend this to timeout, opaque action Busy
  or expected-refusal cases; production mutation retry policy stays unchanged.
- Sparse diagnostic slots can run out across the test binary; module-code
  checks fall back to the safety overflow query.

## Hardware tests

- `pytest_uart_comm.py` requires the separate HIL app; smoke scripts require
  main firmware. Ports and serial traps are in `docs/bench.md`, board state in
  `plans/host-status.md`. Keep
  `firmware/test/uart_comm/protocol.py` in sync with framing changes.
- Restore layouts/settings after smoke checks. Bench tests require normal
  restoration; close the console before cleanup reboots (`docs/bench.md`).
  Use real IDF images for physical OTA tests, never native
  fixtures. Deferred scripts/pass criteria stay in `plans/host-status.md`;
  results go to `plans/hardware-log.md`.

## Validation

- Verify that a check run by CI discovers and executes each new suite. When a
  suite needs a separate check, register it in `tools/validate.py`'s `CHECKS`
  and invoke it from the CI job with the required toolchain. Run checks sequentially.
- Report skipped/unavailable checks. Use `--force` after a toolchain change:
  fingerprints track sources, not tools.
  [The validation guide](../../docs/development.md#validation-toolsvalidatepy)
  covers logs, warning scans and the single compiler-crash retry.
