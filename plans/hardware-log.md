# Hardware log

One short row per hardware run, newest first: the date, the step, what was run (board, firmware, scripts) and the result. Steps append here; sessions read only the rows they need. The bench is described in [docs/bench.md](../docs/bench.md), open deferred checks are in [host-status.md](host-status.md), and the firmware overhaul's results (F1b to LP) are in [archive/firmware-status.md](archive/firmware-status.md#hardware-test-results).

| Date | Step | What was run | Result |
|---|---|---|---|
| 2026-11-15 | H1b | Final read-only state and `bench-stall` refusal | Pass: exact LP ELF confirmed in ota_0; factory loops/params and rates 1000/100/10, watchdog 200 ms, disarmed, idle groups/capture, no pending/dirty flags; normal bench command refused |
| 2026-11-15 | H1b | Final async probe after OTA readiness correction | Pass: 1 kHz latest 190, mapped 190, ring drop-oldest 870 with no ring consumer; keepalives/watchdog/DISARM and complete Boot reload reverified |
| 2026-11-15 | H1b | `ota_smoke.py` with guarded distinct candidate and SHA-verified original LP backup | Pass: duplicate/conflict/gap/hash, reset/FINISH/ABORT, confirmed boot and rollback, exact original full ELF restored in ota_0; NVS restored |
| 2026-11-15 | H1b | Single explicit rollback REBOOT after interrupted OTA | Pass: exact original LP full ELF b8ad3d153466c34d400e15757674e9cd937575b055f1307a658962d514449b94 restored in ota_0 and confirmed; REBOOT was not automatically repeated |
| 2026-11-15 | H1b | First physical OTA attempt (21ba8c8) | Stopped safely with the candidate unconfirmed when Boot interrupted readiness; captured state and fixed host-only Boot classification before the final rerun |
| 2026-11-15 | H1b | `loop_affinity_smoke.py` | Pass: discovered enum/default, choices/refusals/staging/persistence/ticking; factory loop selections restored |
| 2026-11-15 | H1b | `testgen_smoke.py --loop-rate 1000` | Pass: step/chirp, STOP/hold/fresh handoff and watchdog cancellation; original layout/settings restored |
| 2026-11-15 | H1b | `capture_smoke.py --loop-rate 1000 --expect-psram` | Pass: trigger/manual/events/paged reads/abort/refusals and PSRAM; original layout/settings restored |
| 2026-11-15 | H1b | `imu_smoke.py --modules mock` | Pass: signals/calibration/actions/blob, persistence/refusals and saved-state restoration |
| 2026-11-15 | H1b | `motor_smoke.py --modules mock --loop-rate 1000` | Pass: controller velocity/position, mode freshness, watchdog/re-arm and saved-state restoration |
| 2026-11-15 | H1b | `hardware_smoke.py` through the async-backed sync CLI | Pass: discovery, params, layouts, persistence, status/diag and restoration |
| 2026-11-15 | H1b | `time_smoke.py --duration 180` (time-traffic) | Pass: 220 samples, 0 timeouts; held-out p95 209.8 us, prediction-change p95 36.9 us; production generation/host/uncertainty mapping verified; device overruns 0, longest tick 108 us; capture/groups restored |
| 2026-11-15 | H1b | `time_smoke.py --duration 180` (time-idle) | Pass: 220 samples, 0 timeouts; held-out p95 207.8 us, prediction-change p95 29.9 us; production generation/host/uncertainty mapping verified |
| 2026-11-15 | H1b | Preflight and new async consumer on COM3 | Pass: exact recorded LP/clean factory state; 1 kHz latest 188, mapped 188; keepalives/watchdog and fast DISARM, Boot invalidation and complete model reload. Ring intentionally unconsumed, 5237 drop-oldest losses; no firmware change |
| 2026-11-15 | H1a-1 | `ota_smoke.py` with a guarded `ota-image` candidate and a verified backup of the running LP app | Pass: duplicate/conflict/gap/hash checks, partial reset, FINISH/ABORT, confirmed boot, unconfirmed rollback, exact original ELF image restored in ota_0; NVS layout/params restored, disarmed, idle groups, normal bench op refused |
| 2026-11-15 | H1a-1 | `loop_affinity_smoke.py` on COM3, no console | Pass: discovered enum/default, choices/refusals, pending edits, ticking, SAVE/reboot; temporary loops had zero overruns; original layout/core selections restored |
| 2026-11-15 | H1a-1 | `testgen_smoke.py --loop-rate 1000` with mocks | Pass: step/chirp captures, STOP/hold/fresh handoff and watchdog cancellation; regular captures; layout/settings restored |
| 2026-11-15 | H1a-1 | `capture_smoke.py --expect-psram` at 1 kHz | Pass: threshold/manual capture, events, read/abort/refusals and PSRAM; longest ticks 88/90 us for 1/16 refs, both regular; layout/settings restored |
| 2026-11-15 | H1a-1 | `imu_smoke.py --modules mock` | Pass: signals, paused calibration/actions/blob, rejection, SAVE/reboot/profile restore and modes; layout/settings restored |
| 2026-11-15 | H1a-1 | `motor_smoke.py --modules mock --loop-rate 1000` | Pass: cascade velocity/position, mode freshness, persistence, watchdog and re-arm; layout/settings restored |
| 2026-11-15 | H1a-1 | `hardware_smoke.py` through typed CLI on COM3 | Pass: discovery/descriptors, layout, params, SAVE/reboot/reload, actions, status and diagnostics; original layout/rates restored |
| 2026-11-15 | H1a-1 | Preflight and read-only application backup | Initial COM3 absence stopped before changes; user reconnected. Verified normal LP/core v8 and clean loops-only state. Read ota_0, extracted the unsigned app and verified its appended SHA256 and ELF identity for exact OTA restoration. No firmware/runtime changes in this PR; HIL app built only |
| 2026-11-15 | H1a-2 | `model_probe.exe serial://COM3`: baseline, pending mock and final snapshot/reload | Pass: complete discovery, both layout views, all params/enums/limits. Pending mock discarded, saved state restored with REVERT; no flash. COM3 was reconnected by the user before checks |
| 2026-11-15 | H1a-2 | `hardware_smoke.py` on COM3 | Pass: discovery, layout, typed params, SAVE/reboot/reload, actions/status/diag; original layout/rates restored. Initial preflight correctly refused the scratch pending-SET dirty flag; cleanup and rerun passed |
| 2026-11-15 | H1a-2 | `motor_smoke.py --modules mock --loop-rate 1000` | Pass: velocity/position, mode freshness, persistence, watchdog/re-arm; layout/settings restored |
| 2026-11-15 | H1a-2 | `imu_smoke.py --modules mock` | Pass: signals/calibration/actions, SAVE/reboot/profile restore and modes; layout/settings restored |
| 2026-11-15 | H1a-2 | `capture_smoke.py --expect-psram` | Pass: trigger/manual/event/read/abort/refusals, 4 MiB PSRAM; longest ticks 84/102 us with 1/16 refs at 1 kHz, both regular; layout/settings restored |
| 2026-11-15 | H1a-2 | `testgen_smoke.py --loop-rate 1000` with mocks | Pass: step/chirp, STOP/hold/fresh handoff and watchdog cancellation; layout/settings restored |
| 2026-11-15 | H1a-2 | `loop_affinity_smoke.py` and final read-only state checks | Pass: placement enum/default/refusals/staging/persistence/ticking; original LP ELF in ota_0 unchanged and confirmed, layout/params restored, disarmed, idle groups/capture/testgen; normal bench op refused |
