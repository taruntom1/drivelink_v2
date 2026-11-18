# H1b: asynchronous host runtime

Approved by the user on 2026-11-15, from `origin/host_overhaul` at `ebbed1e`.
Branch `codex/overhaul-H1b`, PR target `host_overhaul`. The user approved Codex
planning/building with an independent Codex review at high effort, and explicitly
kept H1b in one PR after the proposed two-part split.

## Approved scope

- Async requests and typed services, a session dispatcher, exactly-once
  reply-order completion, cancellation without undo, mutation uncertainty,
  close/timeout handling and the existing ID retirement/read retry rules.
  Synchronous calls use the same engine.
- Bounded descriptor, capture and OTA transfers. Preserve negotiated payloads,
  cursor dependencies, OTA reconciliation and identical-data WRITE retries.
- Status/core/diagnostic events into the model, dispatched model/status/diag/
  arm/link notifications, Boot invalidation and complete snapshot reloads.
  Expose losses from bounded queues.
- Per-subscription latest seqlock and sample ring; epoch checks, loss counters.
  Automatic prefix-only command keepalives on I/O; map NaN through explicit,
  finite hold values supplied by the consumer. No module-specific semantics.
- Periodic pings (faster initially), sliding-window estimator ownership, reset
  on Boot, mapped timestamp/generation/uncertainty on timestamped data.
- Preserve CLI formats and codes. No firmware/wire change. Identity is H1c.

## Verification and delivery

- Deterministic dispatcher/thread/order/cancel/close/timeout/error/bulk/Boot
  tests; concurrent seqlock/ring coherence and overflow tests; synthetic time
  tests; production vdev consumer at 1 kHz; CLI golden/replay, integration and
  installed consumers on both OSes. Attempt focused Linux ThreadSanitizer.
- Sequential seven-check `validate.py full`, self-review, independent Codex
  review at high effort, fixes and a passing full gate before hardware.
- Bare board: CH343 time idle/traffic with mappings; async/buffer/keepalive
  consumer at 1 kHz; CLI/mock, motor/IMU mocks, capture, testgen, placement and
  OTA smokes. Restore recorded firmware, saved layout/settings and idle groups;
  verify normal bench refusal. Pi timing stays deferred. Stop/report a missing
  port or unresponsive board. No new real-module hardware need.
- Update library/host READMEs, host threading rules, development instructions,
  architecture delivered state, status/handoff and hardware log. Commit/push
  and open one PR into `host_overhaul` with review/full-gate/hardware evidence.

## Implementation choices

- A bounded raw pending-request engine runs on I/O. A serial service executor
  reuses all typed parsers and OTA logic through `Client::async(callable,
  completion)`; the callable owns its arguments and receives a fresh Client.
  No per-call threads or duplicate wire/service parsers. The sync facade waits
  on internal promises, independently of the application's dispatcher.
- Completions use a supplied FIFO dispatcher, or a default serial callback
  executor, and never run inline/on I/O. Admission includes callbacks waiting
  to be consumed; overload throws before acceptance. Accepted completions are
  never dropped. `disarmAsync` bypasses the service queue, reserves one extra
  admission and gets the next TX opportunity after the active frame.
- Descriptor cursors remain sequential (one page); capture READ and OTA WRITE
  use at most four in flight. A batch preflights all bodies/resources.
- Model readers retain immutable shared snapshots, or request value copies.
  Event subscriptions do not consume the legacy event queue. High-rate group
  streams go to pulled buffers, not event observers. Diagnostic queries
  reconcile coalesced counts/active state. Old generations cannot publish over
  a model after Boot.
- Seqlock payload words are atomic to avoid C++ data races. Readers make at
  most eight attempts; contention returns false. Rings drop oldest with a loss
  counter. Managed command producers use a bounded allocation-free queue;
  consumers supply finite hold values. Keepalives never replay value frames;
  stopping, Boot and group replacement invalidate managed state.
- Time pings start after 100 ms, repeat every 200 ms for the first three
  seconds, then every second. Explicit pings can share an in-flight periodic
  ping. CLI uses explicit pings only, preserving recorded wire conversations.
  Uncertainty is an estimate including RTT, residual and age; it is not an
  absolute clock-accuracy guarantee. Irregular captures have no valid
  reconstructed per-row mapping.
- Windows native scheduling can trip the 1 kHz loop's three-period safety
  threshold. The vdev probe measures 1 kHz data while disarmed, then tests
  keepalives at 50 Hz and restores 1 kHz. The bare-board probe tests both at
  1 kHz. No safety threshold or production firmware changes.

## Results (2026-11-15)

- Implemented the approved scope with no firmware/wire change. CLI readable,
  CSV/NDJSON and exit behavior is preserved; time JSON adds production mapped
  host/uncertainty/generation/validity fields. The time smoke verifies them.
- Full validation passed all seven checks on the final implementation, including
  both host toolchains, real-vdev scenarios, CLI golden/replay, installed packages
  and three zero-warning firmware builds. Final documentation commits refresh
  the gate with unchanged checks reused.
- Self-review plus one independent Codex review at high effort. Three valid
  findings were fixed: transport-failure submission liveness, reserved DISARM TX
  capacity, and late Boot reload restoring a lost link. The same reviewer cleared
  the fixes with no remaining actionable findings. No rejected findings.
- GCC 15.2 Linux ThreadSanitizer: 19 concurrency cases and all five model/snapshot
  cases passed without reports. Deterministic scheduling fixtures exercise TX
  saturation and disconnect-before-reload delivery. Temporarily restoring the old
  TX admission/model publication lines made their regressions fail as intended.
- COM3 matched the recorded normal LP image and clean factory state. The new
  consumer passed 1 kHz data/mapping, automatic keepalives, watchdog, fast DISARM,
  Boot invalidation and complete model reload. Idle/traffic 180 s time runs and
  CLI, motor/IMU mocks, capture/PSRAM, testgen, placement and OTA smokes passed.
  OTA used the guarded distinct candidate and verified saved LP application.
  Exact original ELF b8ad3d153466c34d400e15757674e9cd937575b055f1307a658962d514449b94
  is confirmed in ota_0; factory layout, rates 1000/100/10, watchdog 200 ms,
  disarmed state, idle groups/capture and normal bench refusal were restored.
  The HIL app was built only. No new deferred hardware check; Pi time remains open.
- The OTA proxy harness now recognizes its partner thread's intentional socket
  shutdown through a per-connection event. CLI success/identity/confirmation
  assertions remain unchanged.

The first physical OTA attempt exposed Boot interrupting an in-flight REBOOT or
readiness request. The candidate remained unconfirmed. One explicit reboot
rolled back to the exact confirmed LP baseline before debugging. The corrective
host-only `boot_changed` marker lets the CLI resume readiness reads; malformed
protocol errors stay fatal, identity/slot checks stay mandatory, and REBOOT is
never repeated. The proxy regression injects that interruption and asserts one
REBOOT; uncommitted reboot and identity-mismatch cases still refuse confirmation.
The independent reviewer cleared this correction. Full validation and the
physical OTA/async probes were repeated successfully afterward; unchanged clock
and module results above were retained. The vdev probe also applies the existing
ten-attempt test-only group Busy/StagingBusy/Withdrawn policy, excluding uncertain
outcomes, timeouts and other families; production retry behavior is unchanged.

The first PR Linux CI run caught a PTY readiness race: the next CLI's GET_INFO
could be invalidated by a late Boot event after REBOOT acceptance. The integration
harness now polls read-only info at most ten times after an acknowledged reboot,
accepting only connection loss, timeout or the specific Boot interruption. It
never repeats REBOOT or retries scenario mutations/assertions. Independent review
cleared this harness-only correction; production and board results are unchanged.
