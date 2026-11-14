# Plan: automatic initial core placement for loops

Status: implemented and validated, in review. Approved by the user on 2026-11-14.

## Accepted behavior

Newly added loops default to **Any core**. The factory control loop stays on
core 1 and the factory sensor loop stays on core 0. Existing saved selections
of core 0 or core 1 retain their meaning. Users can select any of the three
choices for either real-time or non-real-time loops.

Any core means the task starts without an explicit affinity and FreeRTOS
chooses its initial placement. It is not a balancing policy or a promise that
the loop continues migrating. ESP-IDF 5.5.2 automatically pins a task when it
uses the hardware FPU; `Loop::runTick` already computes floating-point `dt_s`,
so ordinary loop execution is expected to pin it early. The chosen core may
vary across boots, and several Any-core loops may end up on the same core.

Reference: [ESP-IDF 5.5.2, Floating Point Usage](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/system/freertos_idf.html#floating-point-usage).

## Implementation

1. **Expose the choices through the existing descriptor system.**
   In `protocol/defs/loop.yaml`, keep item id `0x0003` and the name `core`,
   change its type from `u8` to the existing `enum` type, and define
   `core_0: 0`, `core_1: 1`, `any: 2`, with default `any`. Keep
   `reboot_required` and persistence. Use the existing enum validator so
   invalid choices are rejected rather than clamped to a valid core. Regenerate
   headers, tables and the descriptor hash with `python tools/gen/gen.py`.

2. **Translate configuration at task creation.**
   Keep the loop's bound storage byte-sized. Set its initializer consistently
   with the descriptor default. Add named placement values and an explicit
   conversion used by `Core::createLoop`: 0 maps to core 0, 1 to core 1, and
   2 to `core_os::kNoAffinity`. Do not pass 2 directly as an OS core id or rely
   on an unsigned-to-signed sentinel conversion. Invalid internal values must
   fail loop startup through the existing failure path. The IDF adapter already
   translates `kNoAffinity` to `tskNO_AFFINITY` and needs no scheduling redesign.

3. **Retain factory placement and configuration lifecycle.**
   Keep the explicit factory core overrides in `Core::boot`. New pending loop
   entries obtain `any` from the descriptor. SET changes only the pending
   selection; SAVE and reboot apply it. DISCARD cancels pending selections;
   REVERT retains its existing live-params-only behavior.
   Retain the current pending-reboot ARM gate, Task WDT registration, heartbeat
   monitoring, priority limits and startup-failure behavior.

4. **Preserve saved settings without changing the blob format.**
   Saved params are item-id/value pairs, and normal SAVE writes every persistent
   param, including `core`. Old saved 0/1 values therefore remain valid after the
   descriptor changes. Verify this with pre-change fixtures and SAVE/reload.
   A stored loop record with no core pair follows the descriptor default, as
   missing params do today; document and test that edge case explicitly. Do not
   erase NVS or add a persistence migration solely for this feature.

5. **Exercise the existing host path.**
   `libdrivelink`, CLI and vdev already discover descriptors and enum names.
   Verify discovery, selection by `core_0`, `core_1` and `any`, numeric selection,
   rejection of invalid values, pending state, SAVE, restart and readback. Make
   host production changes only where these checks reveal an assumption about
   `core` being `u8`; do not introduce a compiled loop-choice table in the host.
   Vdev uses the actual core but its native backend ignores CPU affinity; it
   tests configuration behavior, not ESP32 placement.

## Wire and documentation

This proposal uses existing enum descriptor and scalar encodings, with the same
item id and parameter payload sizes. The descriptor type, enum table, default
and hash change. No new command, signal, status field or descriptor encoding is
needed. On that basis this is a descriptor change, with no core protocol version
bump, like other discoverable module-definition changes. Confirm this against
the final diff; if implementation requires a new wire encoding, stop for the
repo-required wire decision and update firmware, host and Python reference
together. Do not consume the core v9 currently planned for H1b device identity.

Update `docs/protocol/core.md`, generated `docs/protocol/types/loop.md`, the core
and core_os READMEs, and the loop section of `plans/firmware-architecture.md`.
Describe `core` as the configured placement policy, not an observed current CPU.

Record the follow-up in `plans/gui-design.md` and `plans/gui-design-status.md`:
H2 should use the discovered enum choices, show **Any core** for value 2, and
default Add loop to it. The read-only Device table also displays the configured
policy. Keep the FPU behavior in firmware/technical documentation; product help
can say that automatic placement is chosen at startup and can change after a
reboot. Updating Figma screens is a separate GUI design task, not part of this
firmware implementation.

## Validation and acceptance

- Native tests cover all three config-to-task mappings, invalid enum values,
  factory placement with one and multiple loops, new-loop default, pending
  edits, DISCARD/REVERT behavior, safe-mode edits, SAVE/reload, and pre-change saved 0/1 values.
  Observe requested affinity through a small native test seam if needed; do
  not emulate the IDF scheduler or FPU in native tests.
- Host/vdev integration discovers the actual enum table and completes the CLI
  selection/persistence workflow. Existing protocol/descriptor and generator
  tests remain green.
- Run `python tools/validate.py full` sequentially: warning-free IDF builds,
  generated-file checks, native tests, generator tests, Windows/Linux host
  tests and installed-package smoke checks.
- Add a repeatable bare-board smoke script. Refuse pre-existing unsaved/pending
  edits, preserve the saved layout/settings, add temporary loops with mocks,
  exercise both real-time and non-real-time Any-core settings, SAVE/reboot,
  check readback, running state, advancing ticks, diagnostics and stack marks,
  then restore the original configuration. Require more than 1 KiB free stack
  per task and report overrun results for the selected test rates.
- Include an IDF bench observation of requested affinity before floating-point
  work and effective affinity/current CPU after a real loop tick. Use bounded,
  preallocated observations outside logging paths; do not add permanent public
  telemetry for this test. Any starts unpinned, a normal floating-point tick
  leads to affinity 0 or 1, and explicitly pinned loops stay on their selected
  core. Do not require Any-core loops to spread across cores or choose different
  cores on repeated boots. Restore normal firmware after any bench build.
- Re-run existing bare-board mock safety checks with an Any-core real-time loop
  to verify watchdog and stalled-loop handling. Motors, an IMU and a Pi are not
  required. Record hardware evidence separately from vdev/native results.

## Delivery

Implement as one focused PR based on `origin/host_overhaul`, targeting
`host_overhaul`, after checking the prerequisites in `plans/overhaul-status.md`.
Update that status file with the PR, the accepted default decision, validation
and hardware results, and the GUI/H2 handoff. Check whether independent review
is required under the repo's concurrency/persistence review rule before flashing;
use it if the final change cannot be fully covered by the session's own review.

Completion requires the three choices to work end to end, new loops to default
to Any core, factory and saved explicit placements to remain intact, documented
initial-placement semantics, passing validation and bare-board evidence. Runtime
rebalancing and a public actual-core signal are outside this change.

## Results (2026-11-14)

- `python tools/validate.py full`: all seven checks passed, including Linux
  and Windows host unit/integration/installed-package tests and all three
  warning-free IDF builds. The shared placement smoke runs in host integration.
  Linux's first dependency fetch met a network hostname-certificate mismatch;
  the retry used cached Windows archives verified against the repository's
  pinned SHA-256 hashes. TLS verification was kept enabled.
- Self-review plus an independent Codex review found no actionable issues.
  Review focused on enum binding/mapping, saved values, bench snapshot
  publication/lifetime, real-time rules and smoke cleanup. No implementation
  work was delegated. The review completed before flashing.
- `python tools/bench_fw.py bench`: warning-free isolated build and guarded
  flash. `host/test/loop_affinity_smoke.py --cli <cli> --endpoint serial://COM3
  --console COM4 --bench --report <report>` passed. Requested/initial/after
  affinities were:

  | Loop | Requested | Initial affinity | Initial CPU | After affinity/CPU |
  |---|---|---|---|---|
  | Factory control (0) | 1 | 1 | 1 | 1 / 1 |
  | Factory sensor (1) | 0 | 0 | 0 | 0 / 0 |
  | Temporary real-time (2) | -1 (Any) | -1 | 0 | 1 / 1 |
  | Temporary background (3) | -1 (Any) | -1 | 0 | 1 / 1 |

  Both automatic loops ended on core 1: this confirms initial freedom and FPU
  pinning, not balancing. Both had zero observed overruns at 50 Hz; their sampled
  longest ticks were 99/118 us. The smallest observed bench stack was 1756 B.
- `python firmware/test/uart_comm/smoke_safety_firmware.py --port COM3
  --bench --wdt --any-core` passed command/freshness/refusal/fast-disarm checks,
  blockers/fault/recovery/ACK, link watchdog disarm, real-time and background
  stalls, and the Task WDT reboot behavior. Cleanup restored saved placement
  and settings. The WDT panic text was not separately captured on COM4.
- `python tools/bench_fw.py normal` restored a guarded normal image. The shared
  placement smoke with `--console COM4` passed again (no bench flag), with zero
  observed overruns and longest ticks 100/119 us. All ten observed tasks retained
  more than 1 KiB free; minimum 1744 B. Normal `smoke_safety_firmware.py --any-core`
  passed and verified BenchStall refusal. Final `smoke_protocol_firmware.py`
  passed diag v3 queries and time-v1 RX/TX ordering/CRC.
- Final readback matches the original two-loop layout, rates 1000/100 Hz,
  cores 1/0, status 10 Hz and watchdog 200 ms. Disarmed, no pending/unsaved
  edits, idle groups and no pending OTA confirmation. Normal ELF build id
  `b8ad3d153466c34d` (full SHA
  `b8ad3d153466c34d400e15757674e9cd937575b055f1307a658962d514449b94`)
  runs in ota_0, built from the d8b2878 working tree. Descriptor hash is
  `0xDF9F21A5`, core v8 unchanged. COM4 is closed.

The only plan correction was terminology: existing REVERT restores live
parameters; DISCARD cancels pending placement edits. Their behavior was retained
and tested. Figma frames remain a GUI follow-up; the handoff records the discovered
choices, Any-core Add loop default, and configured-policy display semantics.
