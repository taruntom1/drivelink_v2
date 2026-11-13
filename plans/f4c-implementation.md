# F4c implementation plan

Status: implemented and verified on 2026-11-13; ready for review. Based on `origin/firmware_overhaul`
at `717d96e` (F4b PR #47 merged). Codex planning, implementation and an
independent Codex review are approved by the user for this step.

## Scope

Implement the Sources section of `firmware-architecture.md` in one PR into
`firmware_overhaul`: generated source items, source binding and validation,
same-loop sampling, cross-loop snapshots, and `fusion.mock`. Motors, real IMU
drivers, capture and the H1 device model remain in their scheduled steps.

## Changes

- Expand YAML `kind: source` into a persisted, reboot-required u32 parameter
  with `Source` flag 0x80, plus `<name>.value` and `<name>.age_us` signals.
  Validate all generated ids, names and C++ constants, including collisions
  with explicitly declared and retired items. Keep source association derived
  from the generated names; no new descriptor field or protocol operation.
- Fix the packed source reference as `type << 24 | instance << 16 | item_id`
  in `protocol/defs.hpp`, with zero meaning disconnected. Add shared packing
  and unpacking helpers and tests. Existing instance wiring keeps its encoding.
- Bump core wire v4 to v5 in definitions and every consumer, including the
  host, CLI, vdev and Python smoke tools. Document the flag and reference
  encoding in `docs/protocol/` in the same change.
- Add `Sampled<T>` and `InstanceContext::bindSource`. Bind the generated value
  and age signals to the sampled storage. An invalid sample has age
  0xFFFFFFFF; valid age saturates at 0xFFFFFFFE. `fresh` means a new producer
  snapshot since that consumer's previous tick.
- Validate source targets as matching-type signals or inputs, reject target
  removal with `Referenced`, and reject same-loop tick cycles with
  `WiringCycle`. Source links do not become startup dependencies: a failed
  producer leaves the consumer running with an invalid sample.
- Add a boot-built source service with one triple buffer per ordered loop
  pair. Deduplicate identical target refs within a pair. Reuse `BoundSignal`
  and `latchSignals` for producer snapshots; both tick paths use bounded
  work, without allocation, locks, retry loops or waiting. Sample same-loop
  sources immediately before their consumer ticks, after their producer.
- Add `CORE_MAX_LINK_VALUES` (default 32), carry it through `CoreConfig` and
  layout validation, and expose read-only `limits.link_values` at 0x0015.
  The capacity counts distinct target values within each ordered loop pair.
- Add `fusion.mock` with sources for feedback and IMU values, sampled outputs
  and a deterministic combined output. Register it in firmware and vdev.
  Source storage survives arm transitions; preserve F4b's monitor generation
  claim and command-input resets.

## Validation and review

- Generator tests cover expansion, metadata, supported value types, malformed
  declarations and id/name/constant collisions. Protocol tests cover reference
  encoding and the new flag.
- Native tests cover binding errors, layout rejection and rollback, capacity
  and deduplication, same-loop ordering/cycles, disconnected/never-published/
  failed producers, paused-producer aging, freshness, saturation, persistence,
  sparse loop ids and ARM/DISARM retention. A writer-thread stress test checks
  snapshot coherence; the real-time guard covers source transfers and fusion.
- Host unit/integration tests exercise core v5, descriptor discovery, source
  configuration through normal params, reboot/reload and streamed fusion value
  and age signals on vdev. Extend existing suites and build source lists.
- Run `python tools/validate.py quick` during implementation, then `full`
  with every check available and passing. Keep all builds/tests sequential.
- Self-review the complete diff, then use an independent Codex review agent
  at high effort because this step changes wire metadata and cross-loop
  concurrency. Verify findings, fix confirmed issues and rerun validation.
- After review, flash and test the bare board on COM3/COM4. Compare the control
  loop's longest tick with no link and a 16-value outgoing link under matched
  conditions, exercise source streaming and inspect stack marks (>1 KB free).
  Restore the original layout/settings and normal firmware. Stop and report
  if the ports or board are unavailable.

## Documentation and handoff

Update the definitions guide, core wire spec, core and mock READMEs, affected
area rules and host usage documentation. Record F4b as merged, this step's
Codex exception, internal choices and hardware results in overhaul status.
Open one PR, attach the full validation summary, and record its link and the
F5a handoff.

The packed source reference and core v5 changes above are included in this
approval request. No further safety rule or later-phase changes are proposed;
any such discovery during implementation requires a separate user decision.
