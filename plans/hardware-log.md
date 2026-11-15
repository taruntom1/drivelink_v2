# Hardware log

One short row per hardware run, newest first: the date, the step, what was run (board, firmware, scripts) and the result. Steps append here; sessions read only the rows they need. The bench is described in [docs/bench.md](../docs/bench.md), open deferred checks are in [host-status.md](host-status.md), and the firmware overhaul's results (F1b to LP) are in [archive/firmware-status.md](archive/firmware-status.md#hardware-test-results).

| Date | Step | What was run | Result |
|---|---|---|---|
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
