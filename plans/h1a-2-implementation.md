# H1a-2: device snapshot and Windows CI

Approved by the user on 2026-11-15. Base: `origin/host_overhaul` (`32d1041`);
branch: `codex/overhaul-H1a-2`; PR target: `host_overhaul`.
Codex plans and implements, with an independent Codex review at high effort,
under the existing H1a exception.

## Scope

- Add a synchronous, read-only device model over H1a-1's typed services. Its
  initial load and explicit reload read GET_INFO, validate the core version,
  expose safe mode, discover types/items/enums/diagnostic codes, list configured
  and effective pending instances, and read every parameter including limits.
- Key types, instances and items by their actual IDs. Keep system parameters
  separate because the system is not a layout entry. Preserve descriptor flags,
  units, ranges, source and action metadata alongside typed parameter values.
- Assemble the whole replacement before publishing. Failure preserves the
  previous snapshot and propagates the error. This is a host snapshot assembled
  over multiple device reads, not a device-wide atomic transaction. Safe mode
  is readable; loading never exits it, arms or changes configuration.
- Add host-windows CI with pinned/cached Qt 6.12.0 and Qt MinGW 13.1 through
  aqtinstall; run `validate.py --only host-win` and installed consumers. Ensure
  toolchain selection cannot silently use a different compiler.

Async calls, events and automatic refresh, group buffers, keepalives and time
mapping stay H1b. Identity stays H1c. No firmware/wire change or CLI output change.

## Changes and verification

- Library: public model header, loader source and CMake installation; reuse
  Client and existing parsing. Read parameter batches within 64 refs and the
  negotiated payload cap; discover module metadata without compiled tables.
- Host tests: construction/reload against the real-core vdev; safe mode,
  sparse IDs, enums, configured/pending additions/removals/labels/values, limits;
  deterministic peer tests for malformed data, constrained payloads and failed
  reload preservation. Exercise the installed public model API without JSON.
- CI/tooling: workflow, host compiler checks and tests as needed. Register model
  tests within the existing host CTest checks, run both host checks while
  iterating, then the sequential full gate. Inspect current summary provenance
  and `full_gate_passed`; verify the new Windows CI job after pushing.
- Self-review and one independent Codex review at high effort. Fix valid findings
  and rerun affected checks/full gate before hardware.
- After review: existing bare-board CLI smokes and snapshot load/reload on COM3;
  preserve the recorded normal LP firmware, saved layout/settings and idle state.
  Stop/report a missing port or unresponsive board. No new deferred HW checks.
- Docs: library and host READMEs, development/CI docs, architecture's delivered
  state, status/handoff, and actual runs in hardware-log.md. Record internal
  choices here; ask before wire, safety or later-step scope changes.

Finish with the passing full gate, commit/push and one PR into host_overhaul,
its link in the status file, and the next-step handoff.
