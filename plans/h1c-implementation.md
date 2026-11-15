# H1c device identity

Plan approved 2026-11-15. One PR from `codex/overhaul-H1c` into
`host_overhaul`. Codex plans/builds; independent Codex review at high effort
(user-approved model exception). No implementation delegation.

- Core v9 appends factory MAC `device_id[6] | name_len u8 | name` after
  GET_INFO's loop records. System SET_NAME is op 10, empty success body.
  Time v1 and diag v3 remain unchanged; no old-wire compatibility.
- Empty or up to 16 UTF-8 bytes; reject invalid UTF-8, NUL and C0/C1 controls.
  Preserve spaces/case, never truncate. Rename is live, allowed armed, and dirty.
- Name lives in the A/B layout commit record (storage format 3). Read LP's
  format 2 with an empty name to preserve existing layout/settings. SAVE,
  REVERT and factory reset include the name; preserve torn-write recovery.
- Firmware reads MAC at the IDF boundary. Shared codecs/definitions, Python
  references, typed host info/setName, CLI `name [TEXT]`, configurable stable
  vdev id. Model uses explicit reload after mutations.
- Tests: codec bounds/malformed UTF-8, native name lifecycle and failed/torn
  writes, host parsing/model, CLI/vdev persistence; full seven-check gate.
- Self-review and independent high-effort review before hardware.
- Identity smoke on vdev and bare board: stable MAC, rename/clear,
  SAVE/reboot, REVERT, armed rename with mocks; protocol/core/CLI and affected
  service smokes including OTA; stacks >1 KiB.
- User approved leaving verified normal H1c core v9 firmware installed.
  Restore original settings/layout, empty name and disarmed idle state.
- Update protocol/API docs, architecture, status/handoff and hardware log;
  mark H1b merged and record the model exception; push PR after validation.

## Results

Implementation is complete. All production code was written in this session.

- **Wire/API:** core v9, SET_NAME 10, appended six-byte id and u8-length name.
  Invalid names return InvalidArgument/BadValue without mutation. CLI ids are
  12 lowercase hex digits. vdev defaults to `02444c000001`; `--device-id HEX12`
  supports distinct simulated boards. Model refresh after rename/REVERT uses
  its existing reload API. The separate HIL app has no GET_INFO consumer;
  its build passed unchanged. Python reference/smokes use v9.
- **Storage:** layout format 3 commits the variable-length name under its CRC;
  complete-generation selection includes the name. Format 2 loads with an
  empty name. Native tests cover format migration, failed/torn writes at every
  SAVE write, lifecycle, malformed requests and bounds. Factory reset and
  safe-mode behavior are covered natively; no physical factory reset was needed.
- **Review:** self-review plus independent Codex review at high effort, required
  for wire/persistence. One P2 finding (Windows UTF-8 argv reaching ANSI OTA
  file opening) was fixed with `std::u8string`/native filesystem paths and a
  Unicode directory/file regression in the CLI/vdev OTA campaign. The same
  reviewer cleared it. No rejected or unresolved findings.
- **Bench:** verified the original LP ELF `b8ad3d153466c34d` and captured every
  saved loop/system setting before flashing. The 24 KiB NVS backup is
  `build/h1c/nvs-lp.bin`, SHA-256
  `961c43ebed4649a8d732841334689b024edfec9a316bc3a59e9e2f097f129a68`.
  Guarded normal/OTA-candidate builds passed; the LP settings loaded intact.
- **Identity/async:** physical factory MAC `1cdbd446ff38` matched GET_INFO;
  maximum-length ASCII/Unicode names, clear, SAVE/reboot, unsaved reboot,
  REVERT and armed mock rename passed. Production async consumer: 191 latest
  reads, all 191 mapped, no torn values; keepalives/watchdog/DISARM and complete
  Boot reload passed. The unconsumed bounded ring dropped 5464 old samples as
  designed.
- **Service checks:** protocol, raw core, CLI, motor/IMU mocks, capture,
  testgen, placement and OTA passed. Capture at 1 kHz was regular (96/75 us
  longest ticks for 1/16 refs). Testgen step/chirp captures were regular,
  longest ticks 189/267 us; STOP/hold/fresh handoff and watchdog passed.
- **Console recovery:** the first default-control-line console observation
  caused USB reset (reason 11), invalidating cleanup DISARM. The board remained
  healthy/disarmed. Captured evidence, removed only known temporary fixtures,
  and restored the factory layout. The reusable affinity smoke now opens with
  DTR/RTS inactive and runs bounded read-only readiness before cleanup, without
  retrying mutations. Independent review cleared this harness fix. Both the
  standalone and complete console-enabled affinity reruns passed.
- **Stacks:** all observed tasks retained >1 KiB. Console-enabled placement
  run minimum was diag 1752 bytes; isolated maximum-name workload retained
  core_cfg 2632 bytes and main 3552 bytes. Temporary loop stacks: 3168/3192.
- **OTA/final state:** duplicate/conflict/gap/hash checks, partial reset,
  FINISH/ABORT, confirmed boot, rollback and baseline restoration passed.
  Normal H1c ELF
  `af77763ae42b77eaee93f0dd5ce600fbf43d68c0af8956c7b7094b43ecbd08d1`
  is confirmed in ota_0. Original twelve loop/system values match the LP
  checkpoint; empty saved name, no modules/pending/dirty flags, idle groups
  and unconfigured capture, disarmed; both bench-only commands refused.
  Hardware rows: [hardware-log.md](hardware-log.md).
- **Validation:** the full seven-check gate passed again after the reviewed
  console-harness fix, with no omissions and zero-warning builds. The report
  is `build/validate/summary.json`; passing unchanged checks are reused.
  The gate is refreshed for each pushed commit.
- **Windows CI follow-up:** run `43729629264` built successfully but
  `host-identity` tripped the real vdev loop-stall monitor during armed rename.
  The async fixture slowed only its selected loop to 50 Hz, leaving another
  monitored loop at 100 Hz (a 35 ms stall window). The native armed phase now
  uses 10 Hz for every loop and a 1 s command watchdog, restoring and reading
  back all original rates/watchdog after DISARM. The disarmed 1 kHz data phase,
  keepalive/watchdog assertions and physical-board timing remain covered.
  Self-review and the same independent high-effort reviewer cleared the fix.
  Production safety thresholds are unchanged; prior hardware results apply.
  The Windows check passed, followed by five consecutive runs each of
  `host-async` and `host-identity` (10/10 passes), then the full seven-check gate.

Real motor/IMU/Pi checks remain in the existing deferred rows; identity adds no
new hardware requirement. Time v1 is unchanged; protocol pings and production
mapped timestamps passed here, while H1b's full residual/drift results remain
the reference.

## Checklist

- [x] 1 Orient: step, branch, reading, code
- [x] 2 Plan approved by the user
- [x] 3 Final validate.py full gate
- [x] 4 Self-review and independent high-effort review; findings resolved
- [x] 5 Hardware checks and restoration
- [x] 6 Status/docs, commit, push, PR

PR: [#72](https://github.com/taruntom1/drivelink_v2/pull/72), targeting `host_overhaul`.
