# Host rules

Read the root rules, [host architecture](../plans/host-architecture.md) for the
target, and [README.md](README.md) for builds/usage. Until H1a, JSON remains the
public service result; H1a moves it into CLI and preserves CLI output.

- `libdrivelink` is C++20 without Qt/ROS/firmware dependencies. Public namespace
  is `drivelink`; protocol code is `drivelink::protocol`. Asio is private.
  Dependencies stay version/SHA256-pinned; never edit vendored source.
- Discover module types/values/actions from the device, never a compiled module
  table. Generated constants are for core/system IDs only. Share framing,
  Scanner and Reader/Writer; keep descriptor/scalar parsing pure/tested.
  Validate complete lengths/pages/counts and payload limits; mismatches are explicit.
- One Asio I/O thread owns session sockets/serial/TX. Synchronous callers
  serialize; never call the sync facade on that I/O thread. `Client` is
  single-caller; Session is thread-safe. Close/errors wake waiting callers;
  close must not depend on peer progress. Bound event queues, drop oldest and
  expose loss counters.
- Correlate type, sequence, family and op. Retry only known-idempotent reads;
  a mutation timeout has an uncertain outcome. Retire timed-out IDs until
  reconnect; fail explicitly on exhaustion.
- Time ping IDs, boot/session clock generations and timeout retirement stay
  separate from core sequence IDs. Stamp on I/O; ClockEstimator stays
  single-caller, bounded and monotonic. See [time.md](../docs/protocol/time.md).
- OTA's WRITE retry exception permits only identical offset/data. Batch calls
  hold ownership, retire missing IDs and respect device window/chunk/payload
  limits (at most four outstanding). Reconcile uncertain BEGIN/FINISH/CONFIRM
  through STATUS; never automatically repeat REBOOT. A reboot reply is acceptance
  only: verify full running ELF identity, slot and core version before CONFIRM,
  then recheck identity. Cancellation never confirms. See
  [ota.md](../docs/protocol/ota.md).
- vdev uses actual core/mock modules, one config caller, fixed scratch/bounded
  diagnostics and firmware real-time rules. Native storage defaults to in-memory.
  Keep the `-vdev` version marker after simulated OTA so physical preflights refuse it.
- CLI `--json` stdout contains only envelopes/NDJSON; errors retain exit code,
  device result and detail.
- Windows host binaries use Qt's MinGW kit exclusively. Firmware native tests
  may use Strawberry; never share build dirs. The root `full` gate includes
  Linux/MinGW unit/integration and installed-package smoke checks; host-linux CI
  must pass. Hardware smoke runs only on the bare bench board.
