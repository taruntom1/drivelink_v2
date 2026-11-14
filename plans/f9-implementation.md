# F9 implementation plan: OTA, partition table and rollback

Status: approved on 2026-11-14; implementation in progress. Based on `origin/firmware_overhaul`
at `06fef73` (F8 merged). Branch: `codex/overhaul-F9`.

## Approved pre-plan decisions

The user approved these choices on 2026-11-14:

- Codex plans and builds F9, with an independent Codex review at high effort.
- ARM is refused during upload, after FINISH, and while the running candidate
  awaits CONFIRM. ABORT can cancel the prepared update before explicit REBOOT commits it.
- Add read-only OTA STATUS, including progress, boot/confirmation state and
  image identity. Bump the core wire version from 7 to 8. Time v1 and diag v3
  retain their formats.

These choices will be incorporated into the architecture and status file in
the same PR. Implementation starts only after approval of this plan.

## Scope

One F9 PR into `firmware_overhaul`: the dual-slot partition table and rollback
bootloader, pure OTA service and OS adapters, matching SDK/CLI, vdev simulation,
tests, and bare-board update/rollback evidence. Firmware and host change together.
Keep the implementation focused; if it grows beyond one reviewable change
(roughly 4-5k added implementation/test lines), propose a split before proceeding.

Image signing/encryption, persistent interrupted-upload resume, host leases,
GUI integration, and automatic firmware distribution remain outside F9.
An interrupted transfer can be retried while its upload state survives; after
a device reset, start a new transfer. Rollback uses ESP-IDF boot metadata.

## Partition table and initial migration

Add `firmware/partitions.csv`, select it in `firmware/sdkconfig.defaults`, and
enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Keep flash at 16 MiB:

| Partition | Offset | Size |
|---|---|---|
| nvs | 0x9000 | 0x6000 |
| otadata | 0xF000 | 0x2000 |
| phy_init | 0x11000 | 0x1000 |
| ota_0 | 0x20000 | 0x400000 |
| ota_1 | 0x420000 | 0x400000 |

Space from 0x820000 remains unassigned. There is no factory app partition.
Validate offsets, erase/app alignment, non-overlap, image fit, and total flash
size through the IDF build and hardware-free tooling checks.

F8 cannot install this layout over OTA. After review, use the guarded bench
helper for the initial UART flash of bootloader, table, initial otadata and F9
baseline. Never erase all flash or NVS. Snapshot the existing layout/settings
first and verify saved values after migration. Check actual flash capacity and
the booted slot. IDF 5.5.2's bootloader seeds the first OTA slot as VALID when
initial otadata is empty and no factory partition exists; verify that baseline
on the board before attempting the first update.

## Firmware service and wire contract

Add `core/ota_service.hpp` and `src/ota_service.cpp`, owned by Core. Hardware
calls live behind a small `core_os/ota.hpp` interface, with IDF/native backend
sources. Core stays free of IDF types and headers. All OTA calls and state
changes belong to the config caller; flash operations never run in RX, a tick,
or the safety monitor. Fixed chunk/read/hash scratch storage lives off task
stacks. No new task is needed.

Implement family 0x09, generated OtaOp/state/detail definitions, and core v8:

- BEGIN(size, SHA-256): validate the full request and response capacity before
  mutation; require disarmed and a confirmed running image; reject zero or
  oversized images and a different active transfer. Start the inactive slot
  with `OTA_WITH_SEQUENTIAL_WRITES`. An identical repeated BEGIN reports the
  existing progress without erasing it.
- WRITE(offset, explicit chunk length, bytes): accept 1-512 bytes, bounded by
  the declared size. Append only at the expected offset. A fully written
  duplicate succeeds only after byte-for-byte comparison with flash. Gaps,
  conflicting duplicates and partially overlapping writes are rejected without
  changing progress. Return the expected offset on offset/data rejection and
  the next offset on success. Retries never hash or write bytes twice.
- FINISH: require exactly the declared bytes, verify SHA-256 over the exact
  uploaded file bytes, complete IDF image validation and prepare the candidate,
  leaving boot metadata untouched. Repeating FINISH in Ready succeeds without
  another write. Explicit SYSTEM REBOOT selects it after reply drain and then
  immediately restarts; other reset paths never commit a prepared image.
- ABORT: release an active upload or a prepared Ready image without rewriting
  boot metadata. Idle ABORT is idempotent. If a commit has already selected the
  candidate, ABORT refuses (OtaCommitted); reset is required. On an unconfirmed
  running candidate, ABORT refuses; CONFIRM or reset is required.
- CONFIRM: only a host command may mark the pending running app valid. A repeat
  on an already confirmed app is harmless. Release the gate only after success.
- STATUS: available read-only, including while armed or in safe mode. Report
  service state (Idle/Receiving/Ready/Error), expected size/digest and offset,
  slot capacity/chunk/window limits, running and selected boot slots, pending
  confirmation, full running ELF build hash, and the last backend error/stage.

Document exact field widths, op values, state transitions and failure codes in
`docs/protocol/core.md`. OTA mutation requests remain disarmed-only, including
ABORT and CONFIRM. Preserve existing armed checks elsewhere and fast DISARM.

Extend `CommandService` narrowly so documented OTA offset-rejection responses
can carry their expected-offset body; other rejected commands keep their
existing empty-body contract. Validate response space before side effects.
Expose OTA errors through generated diagnostics as well as command results.
Storage/read/boot-state errors must not silently report a safe Idle state.

The OTA ARM gate covers Receiving, Ready, pending confirmation, and uncertain
cleanup/boot-selection failure. Publish its observable state safely to status
reporting; config-only fields stay on the config caller. Report a specific ARM
refusal detail and include the gate in the existing blocks-arm indication.
Failed operations retain the gate until a successful cleanup or reboot resolves
their state. No timeout silently makes an abandoned update armable.

BEGIN does not change NVS. Reject SAVE, REVERT, FACTORY_RESET and layout
mutations during Receiving/Ready to keep the update transaction stable;
read-only operations, fast DISARM, OTA recovery and explicit REBOOT remain
available. Refuse BEGIN while capture or testgen is active rather than claiming
regular acquisition across cache-disabling writes. OTA remains usable in safe
mode. These transaction restrictions will be documented in the architecture.

## Integrity, identity and native backend

Add a small shared, incremental SHA-256 implementation under `protocol/`, used
by firmware/native/host and tested against published known-answer vectors and
split updates. SHA-256 provides integrity, not authentication.

Keep three concepts distinct: the hash of the complete uploaded file, IDF's
app-image digest, and the app descriptor's full ELF build hash. FINISH checks
the first; IDF validates the app; the host compares the candidate app descriptor
with the running full ELF hash after reconnect, alongside GET_INFO/version.
Use bounds-checked parsing of supported ESP32-S3 application binaries; reject
malformed/truncated files before upload. Do not compare the file hash directly
with `esp_partition_get_sha256`, whose app digest has different semantics.

The native backend models two slots and boot metadata, defaults to isolated
in-memory storage for unit tests, and optionally uses the vdev state directory.
It supports deterministic failures and reset transitions: selected candidate
boots pending, confirmation retains it, a subsequent unconfirmed reset falls
back to the last valid slot. Upload handles do not survive reset. Persistent
metadata commits must fail closed on torn/failed writes. Vdev reports simulated
candidate identity but continues executing the same native program; document
that it cannot validate actual ESP bootloader execution or flash timing.

## Host and CLI

Add matching SDK OTA operations and a bounded upload workflow. Add an
OTA-focused bounded batch path in Session for a small number of WRITE requests
in flight (at most four and within the advertised device limit), preserving
ordinary serialized request behavior. Correlate every outstanding response by
type/sequence/family/op; preserve timeout-ID retirement, bounded TX storage,
close wakeups and explicit exhaustion. Window one remains supported.

Retry WRITE only with the identical offset/data under the specified duplicate
contract. Queue-busy and offset replies must not skip an unacknowledged prefix.
Use STATUS to reconcile uncertain BEGIN/FINISH/CONFIRM outcomes; do not extend
automatic mutation retries to unrelated operations. Bound attempts and elapsed
waits. On an unresolved connection loss, report the state as uncertain and
provide a status/abort/reconnect recovery command.

CLI surface: `ota-status`, `ota-upload IMAGE`, `ota-abort`, and `ota-confirm`.
The normal upload command preflights/hashes the image, uploads, finishes,
reboots, reconnects with a bounded deadline, checks GET_INFO/core v8 and full
running build identity, confirms, then verifies the confirmed state. Provide
an explicit no-confirm mode for rollback testing and an upload-only mode for
inspection before reboot. Never confirm merely because a serial port reopened.
Readable progress goes to stderr; JSON stdout remains machine-readable.
Cancellation attempts ABORT while still before reboot and reports any uncertain
cleanup. It never confirms a pending candidate as a cleanup action.

## Changes and validation

Principal paths: `firmware/partitions.csv`, firmware sdkconfig defaults;
`firmware/components/core/` and `core_os/`; `protocol/` hash/definitions and
generated outputs; SDK Session/Client/new OTA source; CLI/vdev; native and host
tests and CMake lists; `tools/bench_fw.py` and its tests for isolated candidate
builds and safe restoration. Update the Python wire reference to core v8.

Cover these with native/host tests:

- Exact lengths, enum/count/overflow bounds, response capacity before mutation,
  complete-file hash, known hash vectors, malformed images and slot overflow.
- Duplicates at old/current offsets, changed duplicate bytes, partial overlap,
  gaps, empty/oversized chunks, repeated lifecycle operations, incomplete or
  corrupted FINISH, ABORT after FINISH, and injected backend failures.
- ARM refusals throughout the transaction and pending boot, successful gate
  release only after cleanup/confirmation, armed OTA refusals, safe-mode OTA,
  and transaction-conflicting operations.
- Confirmed boot, reset without confirmation, reset during upload, process
  restart with persisted metadata, and torn metadata failure handling.
- Window one/multiple writes; lost, delayed, malformed and out-of-order replies;
  queue busy, timeout sequence retirement, close/cancel, reconnect/identity
  mismatch and lost CONFIRM reply. Use the existing frame-proxy test pattern.
- CLI text/JSON contracts and installed-package use; existing core/time/diag,
  control ownership and real-time guards remain passing.

Run targeted checks during implementation, then one sequential
`python tools/validate.py full` with no unavailable checks. Register any new
standalone suite with the validator and CI; otherwise extend the existing
native/host suites. Full validation must pass again after review fixes.

## Review and bare-board checks

Self-review the complete diff, followed by one independent Codex reviewer at
high effort: required for the wire, ARM gate, boot persistence and request
concurrency. Supply this approved plan, exact base/diff and verified results.
Focus on power-loss ordering, slot selection/cleanup, never auto-confirming,
late-reply correlation and real-time boundaries. Resolve valid findings before
flashing.

Add `host/test/ota_smoke.py`, dry-run its shared flow against vdev, then run it
on the bare board after review. Use distinguishable baseline/candidate builds
from isolated directories, with normal bench/XiP guards. Verify:

1. Migration preserves saved layout/settings and starts a valid baseline.
2. Update through CLI, duplicate WRITE acknowledgement, deliberately conflicting
   chunk refusal and incomplete/hash-invalid FINISH; the baseline remains bootable.
3. FINISH/ABORT cancellation leaves old boot metadata untouched and restores ARM availability.
4. Candidate boots pending, blocks ARM, matches its image identity, confirms,
   and survives another reset in the same slot.
5. Another candidate boots without CONFIRM; the next reset returns to the exact
   previous confirmed build. A partial-upload reset preserves the valid app.
6. Existing core/safety/time/CLI smoke tests pass, every observed task stack
   retains more than 1 KiB, and NVS settings survive updates/rollback.
7. Restore normal F9 firmware and the original layout/rates/watchdog/volatile
   state; verify bench commands are refused and no pending confirmation remains.

No motors/IMU/Pi are needed for F9. Stop and report a missing/unresponsive port.
Do not simulate physical power loss by interrupting a bootloader/table flash;
exercise torn writes natively and reset only at controlled application stages.

## Documentation and finish

Update the architecture's OTA/safety sections and host upload contract;
`docs/protocol/core.md`, generated registry/diag docs; root, firmware,
components, main, protocol and host CLAUDE notes as applicable; core/core_os,
host/vdev and host usage READMEs; and bench helper guidance. Keep wire details
in `docs/protocol/`.

After results are known, update `plans/overhaul-status.md`: F8 merged, F9 in
review with its PR, approved model/safety/wire choices, hardware evidence,
current partition/build state and F10 handoff. Preserve existing deferred
hardware rows. Record validation/review/measurements here, commit and push,
open one PR into `firmware_overhaul`, then add and push its status link.

## Review correction R1 (approved 2026-11-14)

Independent review of `457418b` found that Ready ABORT's two IDF metadata
writes can demote the previously confirmed image to NEW if interrupted. A
later reset can then select the canceled candidate. The direct metadata-bit
invalidation alternative falls outside IDF's documented erased-write contract.
No hardware has been touched.

The user approved this correction: FINISH validates and prepares the candidate without
selecting it. SYSTEM REBOOT, after its reply drains, selects the prepared
candidate and immediately restarts. Ready ABORT only releases the prepared
image; it never rewrites boot metadata. STATUS boot_slot remains the actual
selected slot (the running baseline while merely Ready). Once boot selection
has been attempted, uncertain selection holds the ARM gate and requires reset
if the candidate is selected; ABORT cannot pretend to undo that commit.
The CLI reconciles the reboot/commit outcome through STATUS and identity,
without automatically repeating REBOOT. A separate CommitAndReboot after-reply
action prevents EXIT_SAFE_MODE or another incidental reset from committing.
Failure tests cover selection before/after mutation, prepared reset/cancellation,
and an accepted reboot whose commit never takes place.
