# H1a-1: typed services and CLI

Status: approved by the user on 2026-11-15; implemented; in review. Base: `origin/host_overhaul`
(`2793125`); branch: `codex/overhaul-H1a-1`; PR target: `host_overhaul`.

## Approved scope decisions

- The user approved Codex planning and implementation, with an independent
  Codex review at high effort, for H1a (2026-11-15).
- The user approved the brief's split: H1a-1 delivers types, errors, synchronous
  services and the CLI; H1a-2 delivers the device snapshot and host-windows CI.
  H1b and H1c depend on both parts. This session delivers H1a-1 only.

## Scope and API

Replace JSON service results with C++23 types in namespace `drivelink`.
Cover every existing Client family: info, descriptors, configured/pending
instances, layout/system operations, params, actions, diagnostics, groups,
capture, testgen and OTA. Also type status/event and subscription decoding.
Keep synchronous calls and existing transport ownership/retry rules.

- Introduce focused public headers for values, descriptors, service results
  and errors. Use named structs and enums, vectors for pages, optional values
  for absent data, and a variant for decoded event kinds. Preserve unknown
  frames as an explicit raw-frame alternative.
- Represent descriptor-typed scalar values without JSON, preserving bool,
  integer, float and enum semantics. Retain exact slot bits at the wire
  boundary; validated conversion uses the discovered descriptor and enum
  entries. Preserve generic discovery, reference identity, sparse loop IDs,
  action argument/result metadata, flags and pending-reboot results.
- Keep Error kinds and existing CLI exit codes/messages. Add DeviceError for
  device refusals, carrying result/detail and request family/op. Preserve
  request context and outcome_unknown on uncertain mutation failures, including
  errors propagated by OTA; retain the underlying timeout/connection kind.
  Preflight failures must not imply a mutation was sent. Error metadata must
  survive copying/wrapping so H1b can later use value-based completions.
- Parse wire payloads directly into typed results. The library must not retain
  a hidden JSON implementation or add a second compatibility API.
- Move JSON conversion, output field names and presentation names into the
  CLI. CLI control decisions, including OTA verification and testgen/capture
  orchestration, consume typed results. Preserve readable, JSON/NDJSON and CSV
  output, envelopes, errors and exit codes.
- Remove JSON from library headers, linkage and installed package dependencies;
  declare the pinned JSON dependency explicitly for CLI formatting. Verify an
  installed consumer can use the library without JSON headers.

No wire or firmware behavior changes. No async dispatcher, model snapshot,
event-driven model updates, automatic keepalives or time mapping (H1b), device
identity (H1c), or new Windows CI job (H1a-2).

## Files and sequence

1. Before refactoring, build the current host and capture CLI baselines against
   vdev. Add reviewed golden fixtures for readable/JSON/CSV output and failures.
   Normalize only documented nondeterministic fields (timestamps, build identity,
   counters or measured values); compare structure and all stable fields exactly.
   Exercise live services through vdev; use deterministic protocol fixtures for
   output branches that depend on timing or asynchronous events.
2. Refactor `host/libdrivelink/include/drivelink/` and its service sources;
   change `session.cpp` only as needed to preserve request error context and
   mutation uncertainty. Keep retry eligibility, sequence retirement, DISARM,
   serial bootstrap behavior and OTA reconciliation unchanged.
3. Migrate `host/cli/src/main.cpp`; put typed-to-JSON formatting in separate CLI
   files. Update library/CLI CMake, dependency setup and installed exports.
4. Migrate `host/test/` assertions and the external installed consumer to the
   typed API. Register new tests in the existing host CTest suites so host-win
   and host-linux validation both execute them.
5. Update `plans/host-steps.md` and `plans/host-status.md` with the approved split,
   dependencies, model exception, T0 merged state and next-step handoff. Update
   library/CLI READMEs, `host/README.md`, host scoped rule files and relevant
   development documentation. Keep AGENTS.md/CLAUDE.md rule mirrors consistent.

## Verification and review

- Unit tests: scalar boundaries and enum conversion; complete typed payload
  parsing; truncation/trailing bytes; malformed page totals/cursors/counts;
  sparse loop IDs; status/events and stale subscription epochs; capture/sample
  shapes; action blobs; OTA results and errors.
- Error tests: refusal context, sent mutation timeout/disconnect uncertainty,
  preflight failures, read timeouts, close wakeup, unchanged retry policy, and
  metadata preservation during OTA error propagation.
- Run CLI golden checks plus the existing integration, time, group, capture,
  testgen and OTA coverage. Installed-package smoke uses only the typed API.
- Run affected checks while iterating, then `python tools/validate.py full`
  sequentially. Verify current revision/provenance and full_gate_passed in
  `build/validate/summary.json`. Rerun after review fixes as required.
- Self-review and one independent Codex review at high effort, focused on CLI
  compatibility, wire parsing, error uncertainty, DISARM/command paths and OTA
  identity/confirmation. The main session implements; the reviewer does not.

## Bare-board checks and completion

After review, run the existing CLI hardware, mock motor, mock IMU, capture,
testgen and loop-affinity smoke scripts on COM3. Run OTA smoke with the matching
normal baseline and a distinct candidate from `tools/bench_fw.py ota-image`;
use guarded normal builds/restoration if needed. Read the bench guide first,
verify baseline identity before OTA, and restore the saved layout/settings,
normal firmware and idle state described in host-status.md. Stop and report a
missing port or unresponsive board. No new motors/IMU/Pi checks are introduced.
Record actual runs in hardware-log.md and update board state if it changes.

Finish with docs/status, a passing full gate, commit/push, and one PR into
host_overhaul. Put the PR link in the H1a-1 row and hand off H1a-2. If the diff
exceeds the review-size limit despite this split, bring a further split to the
user before expanding scope. Wire, safety or later-step changes also require
the user's decision before implementation.

## Results (2026-11-15)

- Full validation passed all seven checks on 32be684 after fixes; final PR
  revisions rerun the gate and reuse checks whose code inputs are unchanged.
- 36 Windows host unit cases / 1799 assertions passed; host-linux also passed.
  The CLI retains 60 vdev goldens and 12 deterministic replay goldens captured
  from 2793125. Linux additionally checks SIGINT during START, absent status
  and DISARM. Both installed typed consumers pass with JSON discovery disabled.
- Independent Codex review at high effort found two issues, both fixed and
  re-reviewed: preserve null testgen status if interrupted before polling;
  preserve context/uncertainty on malformed service replies, including
  pipelined OTA WRITE. No rejected findings or remaining actionable findings.
  Self-review also separated read classification from retry eligibility and
  preserved uncertainty when OTA reconciliation fails. Retry/command/OTA
  confirmation behavior is unchanged.
- All seven planned bare-board smoke scripts passed after COM3 was reconnected.
  Exact original LP ELF b8ad3d153466c34d400e15757674e9cd937575b055f1307a658962d514449b94
  is restored and confirmed in ota_0, with original layout/params, disarmed state
  and idle groups. Normal firmware refuses bench commands. Results are in
  [hardware-log.md](hardware-log.md); local evidence is in `build/h1a-bench/`.
- No wire, firmware, safety or later-feature behavior change. Approved workflow
  deviations are the Codex model exception and H1a split; H1a-2 is next after
  this PR merges. No new deferred hardware check is needed.
