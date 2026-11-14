# F8 implementation plan (approved 2026-11-14)

Host-initiated time pings with TX-time stamping, a reusable host clock estimator,
and removal of the last legacy manager. One PR targets `firmware_overhaul`
from `codex/overhaul-F8`, based on merged F7b (#57).

## Decisions already approved (2026-11-14)

- Codex plans and implements F8, with an independent Codex review at high effort.
- Bring the reusable clock estimator, ping API and CLI measurement into F8.
  Automatic mapping of timestamped application data remains H1; clock-model
  distribution remains H3. Update both architecture plans and the H1 handoff.
- Malformed requests and unsupported ping versions are dropped and counted.
  Only valid pings receive the planned response; there are no result/detail
  fields. Document this explicit exception to the ordinary command convention.

## Scope and boundary

Implement the ping wire protocol, bounded firmware send-time stamping, host
transport timestamps and correlation, a pure clock estimator, CLI inspection,
vdev parity and hardware checks. Delete `time_sync`, its codec/tests, startup
wiring and Kconfig; retire its diagnostic codes without reusing their numbers.

No new firmware task, persistent state or change to arming, watchdog feeds,
loop scheduling, NVS formats or capture timing is needed. OTA remains F9;
device-model completion, automatic background integration and timestamped-data
mapping remain H1. GUI, agent and ROS work remain in their existing phases.
Keep one reviewable PR; propose a split if the diff approaches the repository's
roughly 4–5k-line threshold.

## Wire and firmware

- Register time request `0x02` (host to device) and response `0x42` (device to
  host), replacing the legacy `0x20` / `0x60` entries.
- Use an independent time protocol v1: request `version u8 | id u16` (3 bytes),
  response `version u8 | id u16 | esp_rx_us u64 | esp_tx_us u64` (19 bytes),
  little-endian. Core remains v7 and diag v3 because their encodings do not
  change. The descriptor hash changes with the generated registry/code table.
- Add shared pure codecs under `protocol/`, including a bounded operation to
  patch the TX timestamp in an encoded frame and recompute CRC16. Validate
  frame type, exact lengths and patch bounds before writing. Ordinary frames
  remain byte-for-byte unaffected.
- Register a small-capture callback in `core/src/idf/core_link.cpp`. Take
  `esp_rx_us` at callback entry, validate the request and enqueue the reply
  directly, bypassing the config queue. Invalid requests increment a new
  generated core counter. Pool exhaustion counts a dropped reply; RX never
  blocks or retries. Reset registration before destroying captured state.
- Add an explicit send-time stamp option to `UartComm` command items. Store
  metadata alongside each preallocated pool entry, initialize it on every
  enqueue and clear it on reuse. Only the TX owner patches the selected frame,
  immediately before its driver write. No deferred callbacks or captured
  object lifetimes are carried into the TX task.
- Preserve command FIFO/stream behavior and existing DISARM handling. Do not
  drain or reconfigure the UART for each ping. Stamp means driver submission,
  not the electrical first/last bit: driver/FIFO backlog, serialization and
  link asymmetry remain measurement uncertainty. Test and document that limit.
- Keep all device timestamps in monotonic microseconds since boot. Remove the
  legacy wall-clock setter and periodic task rather than transferring them.
- Remove startup/dependency/stack-log references, native codec sources/tests
  and legacy-manager documentation. Retire the now-unused time-sync and system
  manager-start diagnostic codes/constants after checking every call site.
  Keep retired diagnostic source/code numbers reserved.

## Host library, CLI and vdev

- Add `Session::timePing()` with a typed four-timestamp sample. Capture host
  monotonic send time on the I/O thread when its queued ping begins writing,
  and receive time when the matching complete frame is scanned, before waking
  the caller. Do not use caller scheduling or subprocess elapsed time.
- Give pings their own u16 correlation and timeout handling. Retire timed-out
  IDs until reconnect, ignore unrelated/late replies, reject matching malformed
  replies or incompatible versions explicitly, and fail explicitly if IDs are
  exhausted. Preserve close/error wakeups, bounded TX, request serialization
  and the single Asio I/O thread. A timeout contributes no estimator sample.
- Add a reusable pure `ClockEstimator` with bounded sample storage and an
  immutable model result: host/device reference times, slope/drift, sample
  counts, fit span, residual and RTT statistics, validity and age. It maps
  device microseconds to the host's monotonic domain; it makes no UTC claim.
- For each sample, compute corrected round trip
  `(host_rx - host_tx) - (esp_tx - esp_rx)` and pair the host and device
  midpoints. Reject invalid order, negative corrected round trip, non-finite
  computations and insufficient/degenerate fit spans.
- Default to a 120-second sliding window with 5-second bins, selecting the
  smallest corrected-round-trip sample in each bin. Use centered linear least
  squares to avoid loss of precision at large host clock values. Require at
  least eight selected samples spanning 30 seconds for a drift-valid model;
  distinguish offset-only warmup from a valid drift fit. Expire stale models
  and explicitly reset on reconnect, device Boot or decreasing device time.
  Expose options rather than embedding bench-specific tolerances in the fit.
- Add CLI `time-ping` and `time-sync [DURATION_S]` with readable and JSON
  results. The measurement command owns sampling: 100 ms during the first
  five seconds, then 1 s. It reports raw timing, model quality, drops/timeouts
  and validity, and handles interruption/connection loss cleanly. There is no
  extra library background thread or silent automatic ping service in F8.
- Extend vdev with the same exact codecs, RX sampling and a queued reply whose
  TX timestamp/CRC are finalized at actual write initiation. Include time
  replies in its reliable-response/deadline handling on TCP and Linux PTY.

## Tests and validation

- Protocol/native tests: exact bytes, lengths, versions, u16/u64 boundaries,
  malformed requests, small buffers, TX patch bounds, stamp changes/CRC
  validity, and stamp metadata reuse through a pure test seam where needed.
- Host math tests: known offset and positive/negative drift; large clock
  origins; jitter/outliers and asymmetric-delay bias; bin selection/expiry;
  insufficient spans; invalid order; stale/reset models and finite mappings.
  Keep these deterministic with synthetic timestamps.
- Host/session tests: timestamps exclude time waiting in the host TX queue;
  correlation, late/duplicate replies, timeout/ID retirement, wrong version and
  length, transport failures and close wakeups. Exercise coexistence with
  ordinary requests and command streams without broadening mutation retries.
- vdev integration: CLI raw pings and short model runs on TCP and Linux PTY;
  malformed-ping drop/count and continued responsiveness; reboot invalidation;
  queued TX stamping/CRC; existing stream, arming, lost-ARM-reply cleanup and
  capture/testgen regressions remain covered.
- Replace the legacy time-sync check in `smoke_main_firmware.py` with the new
  ping contract while retaining its diagnostic checks. Add
  `host/test/time_smoke.py` to exercise the production estimator/CLI and emit
  raw samples and bounded summaries, with configurable bench/Pi thresholds.
- Integrate new tests into existing `gen`, `native`, `host-win` and `host-linux`
  checks; register a new validator/CI check only if a separate suite is added.
  Run affected checks while developing, then `python tools/validate.py full`
  with no omitted checks before review and again after review fixes.

## Review and hardware

Self-review the full diff and run one independent Codex review at high effort
before any flash. Focus its brief on TX pool ownership/reuse, CRC timing,
host timestamp boundaries/correlation/reset handling, estimator numerics and
unchanged safety behavior. Verify findings, fix valid ones and rerun validation.

On the bare ESP32-S3, use `python tools/bench_fw.py normal` and COM3 after
review. Run protocol/CLI/core/safety smoke and a 180-second idle time estimate,
then the same under representative mock subscription/capture traffic using a
single long-lived connection. Use device timestamps/counters for device timing;
record host timing only from the production I/O timestamps.

Proposed CH343 stability gate: at least 95% successful pings in each run; a
drift-valid model after warmup; p95 absolute residual of held-out low-RTT samples
at most 2 ms; and successive mature models, evaluated at the same current
device time, differing by at most 2 ms at p95. Exclude each evaluated sample
from its fit. Record RTT distribution, fit span/drift, residuals, timeouts,
prediction changes and loop overruns; these checks measure stability, not
absolute clock accuracy or a sub-100-us USB jitter guarantee. Do not loosen
thresholds silently if the bench fails.

Observe COM4 stack marks (all tasks retain more than 1 KB) and confirm the
legacy task is absent. Preserve/restore original layout, rates, watchdog and
groups/capture/testgen state; verify normal firmware still refuses bench ops.

Pi GPIO-UART verification stays deferred to HW: the same script accepts the
Pi serial endpoint and a 100-us jitter threshold, reports the exact statistic
used and runs under idle and representative traffic. Dry-run its workflow on
vdev and the CH343 now; add the exact command, wiring prerequisites and pass
criterion to the deferred-check table. The user runs the Pi campaign when
available. A time-exchange test cannot independently prove absolute offset
accuracy or remove unknown path asymmetry; document those limits.

## Documentation and finish

After results are known, update `docs/protocol/time.md`, registry/diagnostic
generated tables, relevant protocol/core docs, UART rules, core/host/library/
CLI/vdev READMEs, root and firmware area rules, and the firmware/host
architecture plans. Remove consumed F8 handoff notes and legacy-manager
claims. Record the approved model/scope/error-policy decisions, F7b as merged,
F8 results and deferred Pi check in `plans/overhaul-status.md`, with F9 next.

Commit and push, open one PR into `firmware_overhaul`, include the final full
validation summary, review and hardware results, then commit/push its link in
the status row. The user approved this plan before implementation.
