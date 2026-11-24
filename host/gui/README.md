# DriveLink desktop GUI

Use the host superbuild for builds and the published guide for operator workflows. The GUI rules own ARM and DISARM authorization, lease handling, callback lifetime, configuration writes and theme constraints. [Host build](../README.md#build) · [GUI rules](CLAUDE.md) · [Operator guide](https://taruntom1.github.io/drivelink_v2/)

## Source map

| Area | QML entry | C++ owner | CTest |
|---|---|---|---|
| Shell, direct connection, Device and faults | [Shell](app/Shell.qml), [Connect](app/ConnectPage.qml), [Device](app/DevicePage.qml) | [Connection](backend/connection.cpp), [device models](backend/device_models.cpp), [safety keys](backend/safety_keys.cpp) | `gui-backend` |
| Agent connection and lease | [Agent tab](app/AgentTab.qml), [lease dialogs](app/LeaseDialogs.qml) | [Remote connection](backend/connection_remote.cpp), [remote state](backend/remote_model.cpp) | `gui-remote` |
| Firmware update | [OTA dialog](app/OtaDialog.qml) | [OTA connection](backend/connection_ota.cpp), [OTA model](backend/ota_model.cpp) | `gui-backend` |
| Layout, live values and actions | [Layout](app/LayoutPage.qml), [action row](app/ActionRow.qml) | [Layout operations](backend/connection_layout.cpp), [live operations](backend/connection_live.cpp) | `gui-backend`, QML layout tests |
| Monitoring and manual command | [Monitor](app/MonitorPage.qml), [command control](style/DLCommandControl.qml) | [Monitor connection](backend/connection_monitor.cpp), [command planning](backend/command_model.cpp), [plot rendering](app/plot_item.cpp) | `gui-monitor` |
| Tune and plant identification | [Tune](app/TunePage.qml), [identification cards](app/IdentifyCards.qml) | [Tune operations](backend/connection_tune.cpp), [session files](backend/tune_session.cpp), [identification](backend/identify_model.cpp) | `gui-tune` |
| Module panels | Layout panel tab | [Panel registration](modules/register.cpp), [BNO055 panel](modules/imu.bno055/README.md) | `gui-panels` |
| Settings and components | [Settings](app/SettingsPage.qml), [gallery](gallery/Gallery.qml) | [Settings storage](backend/app_settings.cpp), [style registration](style/style.cpp) | `gui-settings`, QML component tests |

## Workflow boundaries

- Before changing motion controls, read the authorization, lease-loss and hold rules. The Tune workflows below describe when a device-side run releases its hold. [GUI rules](CLAUDE.md)
- For device requests and delivery semantics, see the library's caller contract. For message details and recovery, use the corresponding protocol specifications. [Library API](../libdrivelink/README.md) · [Protocol specifications](../../docs/protocol/)

## Disconnect

- Disconnect (above Settings in the connected rail) returns to Connect after teardown. Closing the window uses the same warnings and quits after teardown. One Cancel/Disconnect or Cancel/Quit dialog lists every applicable warning.
- `controllingMotion` covers this GUI's ARM in flight or active control. A remote observer can leave while the lease holder keeps driving. Unsaved configuration stays on the device until it reboots.
- The status bar shows Disconnecting… during teardown and keeps DISARM available while there is a link. Disconnect and window close wait for [plant identification](#plant-identification-h2e-3) to write the mode back.

## Device settings

- The Device page's Device settings card edits the system params (issue 105 step 4). Its rows are every `Param` item of the system type that is not ReadOnly and has a value, from the descriptors in the snapshot (`ParamTable::resetSystem`; nothing is hard-coded), ungrouped names first, then a heading per name prefix. The read-only Limits card keeps showing the effective running values. The card has no Save or Discard: the status bar, the banner, the Configuration card and the Discard dialog own them.
- Rows reuse Layout's field, parser, range and note code (`DLParamRow`, `setParamText`, `sendParam`): each SET is sent once, then read back; Esc and DISARM cancel an open edit unsent. Live rows (`status.rate_hz`, `diag.*`, `watchdog_ms` while disarmed) apply at once and show Unsaved until SAVE or REVERT; reboot rows (`limits.*`) stage a value and show "Current 4 → Pending 6 · after reboot". A system param is never staged by being "not configured": only its reboot flag stages it, and its subject is `system.<name>`.
- Locks follow one rule with Layout (`rebootBlocked()`, `liveParamBlocked()`): armed locks `watchdog_ms` and every reboot row; an observer of another client's lease cannot stage a reboot row (row note "Needs the control lease", tooltip with the holder), while its live rows wait for the Confirm live edit dialog.
- Layout counts use the pending limits (`pending_system_params`, the running ones when absent), because the device checks layout ops against those. A limit SET below what the pending layout uses is refused with Capacity ("The pending layout uses more than this").
- Safe mode runs reboot rows at their defaults while GET_PENDING keeps the saved values, and that difference alone does not set the device's reboot flag. Such a row is not pending: it shows the saved value with the neutral note "Safe mode runs 4 · 6 after a normal boot", the "N pending" badge leaves it out, and `pendingChanges(snapshot, safeMode, pendingReboot)` lists system differences only once the device reports a pending reboot (the rows are then violet, and both keep the same label, because it cannot tell saved from staged per row).
- SAVE then DISCARD leaves the device's reboot flag set with nothing different in GET_PENDING and nothing unsaved. `deviceSettings` carries both flags: `savedThenDiscarded` (card note and banner "Saved settings are waiting for a reboot." with Reboot; Save and reboot stays available and keeps the values shown) and `discardAfterSave` (extra Discard dialog text: the next boot still loads what was saved).
- Tests: `deviceSettingsLogic`, `deviceSettingsOperations`, `deviceSettingsInSafeMode` and `deviceSettingsInShell` in `gui-backend`, `deviceSettingsThroughAgent` in `gui-remote` (Linux), the row notes in the QML layout tests, and the `device-settings` screenshot scenario.

## Tune (H2e-1)

- Discover running controllers with a testgen-capable f32 input. The gain editor uses live descriptors flagged `tune`. Gain drags coalesce to at most one send per 50 ms with a final send on release, and only one write per gain is in flight. Saved gain values are known only after a clean configuration read. Revert waits for pending writes and sends one live batch.
- A run requires this window's accepted ARM and ready command groups. Hold manual commands at zero and send capture CONFIG, capture ARM and TESTGEN START once. A refusal aborts a capture configured by that run. A missing START reply sends STOP.
- Release the hold after TestgenDone or STATUS establishes that the signal ended. When STOP is pending, wait for it to land and for the signal to stop. A fresh frame returns a commandable target to zero. Stop retains the capture, and DISARM marks it as cut. Boot, link loss and close end the run without further device calls.
- Lock mode, Revert and Restore during a run. With Auto re-run disabled, lock gains too.

## Tune session (H2e-2)

- Auto re-run uses the Run path while Tune is visible, after gains settle for 0.5 seconds and differ from the last run. It requires an explicit Run under this window's current ARM. DISARM, faults, Boot, link loss or changing controller require a new Run. A gain change during a run sends STOP and ABORT once, discards the run and holds the controller at zero until STATUS confirms the signal stopped.
- Opening a session changes only local state and requires a new Run before Auto re-run. Restore matches controller type, label and gain names, then checks value types and ranges. Session files use versioned JSON with validated input, and imported strings render as plain text. [Session codec](backend/tune_session.cpp)
- Recovery autosaves a copy after one second of changes and writes immediately on device change or close. A recovered file stays available until the operator chooses Restore or Discard, except that a newly recorded run can replace it. Explicit save, open and discard fence pending writes before removing the recovery file.

## Plant identification (H2e-3)

1. Require the operator's confirmation that the actuator can move freely and this window's accepted ARM. DISARM, controller change and reconnect clear confirmation. Identification requires a commandable input and does not grant permission for Auto re-run. [Module naming conventions](../../docs/adding-a-module.md)
2. Hold manual commands at zero. Read the current mode from the device, write the selected open-loop mode if needed, and stop setup on refusal. Send CONFIG, ARM and START once. START requires the same ARM generation that authorized the step.
3. After the signal stops, send a fresh zero before releasing the hold. Restore the previous mode while the controller is still held. A local configuration block delays write-back. An unexpected boot or link loss can interrupt write-back. On link loss, report that the mode may still be set.
4. Block gain and mode edits, Restore, Revert, Run, Auto re-run, Open, Save, reboot, Rename and firmware operations until write-back finishes. Disable Disconnect with an explanation, and answer a window-close attempt with a notice to try again after cleanup. Stop and DISARM stay available. Keep the current Tune tab during this interval.
5. Fit responses on a worker, using the captured input transition to end the fit window. Only the operator's Apply writes suggested gains, as one live batch with a history entry. Suggestions require an open-loop step, a settled response, a steady start, a usable fit and positive gain.

## Verification and packaging

Run `ctest --preset linux -R '^gui-'` from the superbuild, or use the Windows preset. Filter a QtTest executable by test function when isolating a failure. The development guide describes virtual-device coverage and the packaged GUI smoke checks. [GUI checks](../../docs/development.md#gui-shell-checks-h2a-2)

The style gallery runs interactively or writes both themes with `drivelink-style-gallery --out shots/`. Screenshot tests use the offscreen software renderer. Wait for `waitForItemPolished` before clicking inside a newly opened popup so its positions have settled.

| Explicit bench case | Inputs and prerequisites |
|---|---|
| `benchBoard` | `DRIVELINK_GUI_BENCH_ENDPOINT`. Normal firmware on the bare board, with no pending or unsaved edits. The case restores layout and name. |
| `benchOta` | Endpoint plus `DRIVELINK_GUI_BENCH_OTA_IMAGE`, optionally `DRIVELINK_GUI_BENCH_OTA_RESTORE` to reinstall the original app. Start with clean saved configuration. |
| `benchMonitor` | Endpoint. Compares delivered sample rate and losses at several loop rates and restores the board. |
| `benchTune`, `benchIdentify`, calibration panel | Tune requires the endpoint and a mock fixture flag (`DRIVELINK_GUI_BENCH_STAGE_MOCK=1`) or a selected controller and target. Identify requires the endpoint and `DRIVELINK_GUI_BENCH_IDENTIFY`. Read the test functions for optional inputs and the hardware checklist for operator prerequisites. [Hardware checklist](../../plans/host-status.md#deferred-hardware-checks) |

Use the installer validation check for Windows staging and Inno Setup. Use the Debian and installed-package checks for Ubuntu packaging. Packaging commands and toolchain prerequisites are in the validation matrix. Distribution and licence rules live in the host rules. [Validation matrix](../../docs/development.md#validation-toolsvalidatepy) · [Host rules](../CLAUDE.md)

## Shell screenshots

The app's `--out DIR` writes four PNGs (2560×1440 and 1920×1080, Dark and Light), waits for live state with a timeout, then disconnects. `--screen` accepts `connect`, `device`, `device-settings` (Device scrolled to the Device settings card), `layout`, `tune`, `monitor`, `drawer`, `dialog`, `rename`, `reset`, `unsaved`, `notice`, `layout-review`, `layout-add-instance`, `layout-add-loop` (with `--select TYPE:INSTANCE` for the Layout screens), `monitor-paused`, `monitor-budget` (with `--plot KEY,...`, `--rate LOOP:DIVIDER,...` and `--window S` for the Monitor screens) and, with `--image FILE`, `update` (the image choice), `update-writing`, `update-confirmed`, `update-rolledback`; `--expect-state` accepts `ready`, `fault`, `safe`, `lost`. Except for Connect, pass an endpoint. `--settle MS` waits before capturing (loop run time needs about a second); `--settings FILE` isolates recent devices. It does not inject faults or provision a supplied device; `unsaved` and `notice` rename (and reboot) the device, so they accept only a `tcp://` vdev endpoint, as do the `update-*` screens (they install firmware).

Remote screens (H3b-1) go through `drivelink-agent`: `--agent HOST:PORT --token T --device ALIAS` (`--client-name`, `--take-lease`) with `device`, `tune`, `monitor` or `drawer`, and `agent-connect` (the Agent tab's device listing; `--agent`/`--token` optional), `agent-failure` (the failed connect), `lease-take`, `lease-release`, `revoked` (these sit on Device, or on Tune with `--tune`) and `live-edit` (`--live-edit KEY=VALUE`, on Tune with `--tune`, else Layout). With `--take-lease` the app prints `GUI LEASE MINE` once the lease is this window's so a script can sequence another client. The Linux-only `host/gui/tests/remote_scenarios.py` (not a ctest) provisions one sample vdev, starts the agent and raw clients that play ROS or a second GUI (`agent_integration.RawClient`) and renders sixteen G6 states into `OUT/<scenario>/`; in WSL: `python3 host/gui/tests/remote_scenarios.py --gui $B/gui/app/drivelink-gui --vdev $B/vdev/drivelink-vdev --agent $B/agent/drivelink-agent --out DIR [--only NAME...]`.

From the repo root, the isolated Windows vdev campaign is:

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.12.0\mingw_64\bin;' + $env:PATH
python host/gui/tests/scenarios.py --gui host/superbuild/build/windows-mingw/gui/drivelink-gui.exe --vdev host/superbuild/build/windows-mingw/vdev/drivelink-vdev.exe --backend host/superbuild/build/windows-mingw/gui/gui_backend_tests.exe --out build/gui-scenarios
```

The script defaults to offscreen/software rendering, runs the backend checks when `--backend` is supplied, and renders thirty-nine scenarios, the last four the firmware update screens (it writes a non-executable vdev image for them), into separate folders (156 PNGs, dimensions checked). The Layout and Monitor scenarios first build the G7b three-loop sample on vdev with `drivelink-cli` (next to vdev, or `--cli`), save and reboot it, and stage changes for the pending ones; `--only NAME...` picks some. `--smoke` renders only Connect and Device; CTest uses that subset on both OSes. Use the CTest-resolved executable paths on Linux (`ctest --preset linux -N -V -R '^gui-'` from `host/superbuild`).
