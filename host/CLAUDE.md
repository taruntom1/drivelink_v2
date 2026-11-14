# Host rules

The root CLAUDE.md holds cross-area rules. The architecture target is
plans/host-architecture.md; F3c is the first service slice, not the full H1 model.

- `libdrivelink/` is C++20 with no Qt, ROS or firmware dependencies. Its public
  namespace is `drivelink`; protocol code stays in `drivelink::protocol`.
- Use the shared framing, scanner and Reader/Writer. Module descriptions,
  values and action signatures come from the connected device, never a compiled
  mock/module table. Generated headers are for core/system IDs only.
- One Asio I/O thread owns a session's sockets, serial port and TX queue.
  Synchronous request callers serialize. Never call the synchronous facade on
  its own I/O thread. `Client` calls require one caller; Session is thread-safe.
- Requests correlate message type, sequence, family and op. Retry only known
  idempotent reads. A mutation timeout means its outcome may be unknown. Retire
  timed-out IDs until reconnect; fail explicitly after all IDs are retired.
- Receive event queues are bounded and drop oldest with a visible counter.
  Socket errors and close wake waiting callers; no peer-dependent wait in close.
- Validate complete response lengths, page progress, counts and payload limits.
  Do not reinterpret structs as wire data. Keep descriptor/scalar parsing pure
  and covered by host tests. A protocol mismatch must be explicit.
- vdev uses the actual core and mock modules, one config caller, fixed core
  scratch buffers and bounded diagnostics. Keep real-time firmware rules when
  touching native core_os. The native test backend remains in-memory by default.
- Dependencies are version- and SHA256-pinned in `cmake/Dependencies.cmake`.
  Standalone Asio is private; nlohmann JSON is a public service result type.
  No vendored dependency source edits. Builds treat warnings as errors.
- Host binaries on Windows use Qt's MinGW kit exclusively. Native firmware
  tests may retain Strawberry's separate toolchain. Never share binary dirs.
- Definition of done: host unit/integration and installed-package smoke pass
  on Linux and MinGW locally; host-linux CI passes, existing required firmware,
  generator and native checks pass, and docs/status update in the same PR.
  `python tools/validate.py full` runs all of these locally (`host-win`,
  `host-linux` through WSL, and the rest); `quick` leaves out IDF and WSL.

The CLI supports readable output and `--json`; stdout in JSON mode contains
only envelopes/NDJSON. Errors include an exit code, device result and detail.
Hardware tests use host/test/hardware_smoke.py only on the bare bench board.

F8 time pings use separate u16 IDs and I/O-thread timestamps. Keep Boot/session
clock generations and timeout retirement distinct from ordinary core sequence
IDs. ClockEstimator is single-caller, bounded and monotonic; H1 will integrate
periodic ownership and application-data mapping. Wire/bias limits are in
`docs/protocol/time.md`.

F9's OTA service is the explicit retry exception for WRITE: only the same
offset/data may be retried under the device's duplicate contract. Session's
OTA-only batch path holds call ownership, keeps at most four requests in flight,
and retires missing IDs. Respect both advertised window and payload/chunk limits.
Reconcile uncertain BEGIN/FINISH/CONFIRM through STATUS; never broaden mutation
retries elsewhere or automatically repeat REBOOT. A reboot reply proves only
acceptance. Verify full running ELF identity, slot and core version before
confirmation and recheck identity afterward. Cancellation never confirms.
Vdev must retain its -vdev version marker after simulated updates so physical
hardware preflights still refuse it. See `docs/protocol/ota.md`.
