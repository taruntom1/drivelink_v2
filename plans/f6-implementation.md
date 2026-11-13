# F6 implementation plan

State: approved by the user on 2026-11-13; implemented and independently reviewed; bare-board checks passed; [PR #55](https://github.com/taruntom1/drivelink_v2/pull/55) in review.

## Approved pre-plan decisions

- Codex plans and builds F6 in this session, with an independent Codex review at high effort.
- Calibration persists on the board and is restored at boot. `calibration_read` and `calibration_write` update RAM and mark configuration dirty; explicit `SAVE` commits it with the layout. Calibration actions do not write flash.
- A runtime IMU read failure reports a diagnostic and invalid data; consumers choose their response. The existing startup-failure arm gate and loop-stall monitor remain in force.
- Preserve the legacy data coverage and selectable operating modes, with discovered signals and explicit units.

## Scope and deliverable

One PR from `codex/overhaul-F6` into `firmware_overhaul`: add `imu.bno055`, its native fake-bus tests, persistent calibration and matching mock actions; delete `imu_bmo_055` and its private protocol. F5b is already merged and will be recorded as such.

This does not add capture, testgen, time ping, OTA, GUI panels or ROS adapters. Real-IMU checks are scripted and deferred to HW. The implementation is expected to fit one reviewable PR; most deleted lines are the old manager and vendor driver. If the new implementation exceeds the repository's roughly 4–5k-line review threshold, propose a split before expanding it.

## Module and driver

- Add `firmware/components/mod_imu_bno055/` with public module/bus interfaces, private driver details, pure implementation, an IDF bus backend, Kconfig, explicit CMake sources and a short README. Register the hardware type only in firmware.
- Define `protocol/defs/imu.bno055.yaml` with an unused type id, proposed `0x30`, and `realtime: forbidden`. The default compatible loop is the factory sensor loop. Bind storage before acquiring hardware; failed setup releases resources and reports the type's code-1 `InitFailed` with named stages and the underlying error.
- Use an instance-owned register driver over a small read/write/delay bus interface, replacing the old global Bosch-driver state. Keep all device/OS headers in the IDF backend. No new task, UART callback or private stream. Native tests substitute a fake bus; vdev continues using `imu.mock`.
- Use the IDF master-I2C API with setup-only allocation, bounded transfers and deterministic release. Claim controllers centrally; each instance owns its bus and its SDA/SCL pins under the existing exclusive GPIO policy. Shared-pin buses and a general bus module are outside this PR. Address, bus speed, timeout and pins have explicit bounds; hardware configuration is structural. Unset pins fail start cleanly. No fallback from a real type to synthesized data.
- Preserve CONFIG and the twelve sensor/fusion modes. Persist the startup mode; provide an acknowledged paused action for a live mode change, with an actual-mode signal and explicit failure reporting. Bus I/O never runs in a parameter change hook.
- Expose Euler angles and quaternion; linear acceleration, gravity, accelerometer, gyroscope and magnetometer vectors; temperature and four calibration levels. Use radians, rad/s, m/s², microtesla and degrees Celsius, and document the sensor coordinate convention. Indicate which outputs are meaningful in the selected mode.
- Publish complete samples only after a successful read. Keep previous numeric values on failure and expose validity, sample count and sample age so retained data cannot appear fresh. Cross-loop source age still describes core publication age; consumers needing sensor freshness must use the sensor validity/age signals.
- Follow the Bosch datasheet for identity, register pages, unit selection, startup readiness, mode settling, calibration access and error handling. Poll/retry only within explicit bounds. Prefer a single bounded burst for sample data/status. Check behavior against the current loop-stall deadline; do not extend or suppress the monitor for bus I/O. Document the supported/default 100 Hz usage and behavior of excessively fast sensor loops.

## Calibration and core integration

- Keep `calibration_read`, `calibration_write` and `calibration_apply` as `runs_in: paused`. Read captures hardware calibration into the staged configuration and returns it; write validates and stages a host blob; apply sends the staged configuration to the sensor. Applying without valid calibration fails explicitly.
- Specify a versioned, exact-length action blob containing the legacy offsets, radii, optional SIC matrix and its presence flag. Encode field by field; reject malformed versions, flags and lengths before mutation. Use the same codec and semantics for `imu.mock`.
- Store the calibration as named typed persistent parameters, including a validity marker, within the existing per-instance param blob. Preserve instance format 1, layout format 2 and the existing complete-generation A/B commit algorithm.
- Add a bounded internal mechanism for successful paused actions to request an atomic update of their own live parameters. Core validates the complete update before publishing it, keeps bound storage and configuration shadows coherent, marks NVS dirty, and respects pending-layout SAVE behavior. Do not let a module directly edit config-task shadows, issue arbitrary cross-instance writes or bypass structural/armed restrictions. Keep this metadata internal to the C++ action API; it does not change action wire encoding.
- SAVE, REVERT, removal, factory reset and failed/offline instances follow the ordinary parameter lifecycle. REVERT restores staged calibration; it does not silently apply it to the running sensor. A subsequent apply or reboot uses the restored values. Boot applies valid saved calibration before entering the configured operating mode; restore failure fails that instance with an actionable diagnostic.
- Extend `imu.mock` with matching calibration state, persistence, actions and representative signals while preserving its existing rate, yaw, sample/reset IDs and fusion behavior. Its actions/ticks remain deterministic and allocation-free.

## Wire and legacy removal

- Add the new descriptors and instance diagnostic codes; retire every old `imu` code while retaining source `0x12`. Reserve manager id 3 and started bit `0x04`.
- Remove legacy `0x22`, `0x42` and stream `0xBF`; increase the command-channel count and `CORE_COMMAND_GROUPS` maximum from 31 to 32, retaining default 8. Leave `0x42` unassigned until F8.
- Delete the old manager, vendor driver, codecs, Kconfig and legacy codec tests. Update main startup/registration, task-name logging, native build lists, smoke scripts and registry/generator assertions. Keep legacy time_sync operational.
- Retain core wire v5 and diag v3: existing framing, codecs and descriptor encoding are unchanged. New type/items/actions are discovered through the existing protocol, and the descriptor hash changes. Document every new module/blob contract and the freed channels in the same PR. Any encoding change discovered during implementation returns for user approval.

## Verification

- Native driver tests use a register-level fake based on Bosch's datasheet: identity/readiness, pages, mode changes and minimum settling delays, signed decoding and unit conversion, every signal family, per-mode availability, NACK/timeout, bounded retries and complete-sample publication. Cover independent instances/resource ownership and failure cleanup.
- Calibration tests cover exact blob validation, no mutation after rejected writes, missing staged calibration, CONFIG-mode access, mode restoration on success and failure, apply/read errors and recovery.
- Core tests cover the paused-action parameter update contract: all-or-nothing validation, ownership, shadows, dirty state, pending layout, armed restrictions and unchanged loop/RX behavior. Exercise SAVE/reboot restore, REVERT/apply, removal/id reuse, factory reset, corrupt/torn writes, incomplete generations and boot restore failure.
- Host integration runs the same new `host/test/imu_smoke.py` against vdev using `imu.mock`: discovery, typed signals, subscription groups, calibration read/write/apply, malformed blobs, SAVE/reboot/restore and cleanup. Preserve existing mock/fusion coverage. Cover both Windows and Linux transports through the current integration suite.
- Add these cases to the existing native/host suites and validation inputs. If a separate suite is needed, register it in `tools/validate.py` and CI.
- Run targeted checks while building, then `python tools/validate.py full` with nothing omitted: generator/tooling tests, native tests, Windows/Linux host tests and all three zero-warning IDF builds. Run one validation/build at a time and retain bounded summaries/full logs.

## Review and hardware

- Self-review against the approved plan and repository invariants, then an independent Codex review at high effort. It is required because this change touches persistence, paused-action concurrency and the registry. Review focuses on transaction atomicity, loop ownership, restore/error paths, stale data and bus deadlines. Resolve valid findings and rerun full validation after fixes.
- After review, flash normal firmware using `python tools/bench_fw.py normal`. On the bare board, run the real module's missing-sensor path on pins 8/9: bounded failure, type-specific diagnostic, failed instance visible and ARM refused. Remove the test instance and restore the original saved layout/settings.
- Run the mock calibration/subscription/persistence smoke on the board plus core, safety, host and remaining legacy smoke checks. Check main, sensor/config and other task stack marks remain above 1 KB. Use device timestamps/counters; avoid holding COM4 open across repeated COM3 resets.
- The IMU smoke script has mock and real flags. Dry-run its workflow on vdev and the bare board with mocks, and test the real missing-sensor refusal path. Record a deferred real-BNO055 command with address/pins and explicit pass criteria: discovered signal groups, plausible stationary values, calibration read/write/apply, SAVE/reboot restore and mode recovery. Physical calibration quality remains unverified until a sensor is attached.

## Documentation and finish

Update the firmware architecture plan for persistent calibration and the chosen bus/action contracts; the host plan where its F6 expectations change; `docs/protocol/` and generated tables; root/firmware/component/main/test guidance; the core and mock READMEs; the new component README; host/vdev usage where needed; and the time_sync-only legacy-manager guide.

After review/hardware results, update `plans/overhaul-status.md` with the Codex exception and user decisions, F5b merged, F6 results/deferred check, current board state and the F7a handoff. Commit/push, open one PR into `firmware_overhaul`, include the final full-validation summary, review findings/dispositions and hardware results, then record the PR link and push it.

Design reference: [Bosch BNO055 datasheet, revision 1.8](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bno055-ds000.pdf).

## Results

- Full validation passed on reviewed code commit `855efcb`; final docs also run
  through the generator check before the PR. All seven checks selected.
- Independent Codex review at high effort found health/startup, subscription
  bound, I2C settings/tick-rounding and smoke-status/calibration issues, then a
  temperature-unit mask error. All were verified and fixed; none rejected.
- Normal F6 firmware flashed via the guarded bench helper. Bare-board mock
  workflow and real missing-sensor/ARM refusal passed, plus legacy/core/safety/
  CLI smoke and final restoration/bench refusal. Captured stacks all >1 KB
  (minimum 1580). COM4 was closed before cleanup reboots.
- Physical BNO055 signals/calibration remain deferred, with the same script's
  real mode and an explicit full-calibration precondition.
- Handwritten additions remain one reviewable PR; generated descriptor growth
  and legacy/vendor deletion account for most of the overall diff.
- CI then exposed an existing group-slot contention rejection before the IMU
  scenario. A bounded test-only retry handles group StagingBusy/Withdrawn;
  production CLI/SDK policy is unchanged. New harness unit tests and focused
  independent review cover no-effect retries versus unknown outcomes.
