# drivelink-gui

The PC GUI: Qt 6 and QML, built from the Figma design
([gui-design.md](../../plans/gui-design.md), node ids in
[gui-design-status.md](../../plans/archive/gui-design-status.md)). Windows uses Qt
6.12.0 with Qt's MinGW 13.1; Qt 6.10 (Ubuntu 26.04) is the API floor that the
Linux build enforces. The superbuild builds it unless `DRIVELINK_BUILD_GUI=OFF`
(the `linux-no-gui` preset). colcon ignores this directory.

The app supports direct connections and remote operation through the agent.
Its pages cover firmware updates, layout editing, monitoring and manual
commands, tuning and plant identification. Custom panels provide controls
for a module type, such as the BNO055 calibration panel.

| Path | What |
|---|---|
| `theme/` | `tokens.json` (Figma export), `gen_theme.py`, generated `Theme.qml`, `export_tokens.js` |
| `resources/` | Inter, JetBrains Mono, the Lucide icons, their licences ([SOURCES.md](resources/SOURCES.md)) |
| `notices/` | third-party notices: manifest, licence texts, generator, the generated file (H2g) |
| `installer/` | the Inno Setup script (H2g) |
| `style/` | QML module `DriveLink.Style` (static library `drivelink_gui_style`) |
| `gallery/` | `drivelink-style-gallery`: every control state, Dark and Light |
| `backend/` | `Connection`, `LocalConnection` (direct or through drivelink-agent), `SafetyKeys`, generic Qt models and command planning |
| `app/` | `drivelink-gui`, static QML module `DriveLink.Shell`, screenshot mode |
| `tests/` | `gui-quick`, `gui-gallery`, real-vdev `gui-backend`, `gui-screenshots`, scenario script |

For released packages, see [Install the app](https://taruntom1.github.io/drivelink_v2/pc/install/) and
[Desktop app screens](https://taruntom1.github.io/drivelink_v2/reference/gui/).

## Application and backend

Run `drivelink-gui`, or pass `--endpoint tcp://127.0.0.1:8765` (vdev) or
`--endpoint serial://COM3`. A development build needs Qt's MinGW and Qt `bin`
directories on PATH; the installed copy is self-contained (H2g, below). The
Windows executable is
`host/superbuild/build/windows-mingw/gui/drivelink-gui.exe`.

`Connection` is the Qt-facing interface. `LocalConnection` opens a Session and
loads metadata on a worker; completions and model events reach Qt through the
session's FIFO dispatcher. Properties, model notifications and UI signals run
on the GUI thread. Immutable snapshots and connection/session generations
prevent late results from reviving a closed or rebooted connection. The
application owns the dispatch receiver; close drains accepted callback chains
before returning. Initial event draining is bounded and checks cancellation.

DISARM uses `Client::disarmAsync`, independently of the ordinary service queue,
including during discovery and an outstanding ARM. A request before session
publication is retained until the link opens; closing before it can be sent
reports cancellation. A later explicit DISARM is retained even if the first
request fails. Observer close waits for an admitted DISARM and reports its
result. Forced close without confirmation preserves an uncertain outcome.
ARM requires current complete metadata, status and command-group readiness;
generation/fence checks and compensating DISARM guard late ARM completions.

Direct connections allocate only free command slots and running, discovered
inputs, split by loop and the device's entry/payload limits. With no inputs,
an empty group on a running loop supplies keepalives. Safe mode, occupied
inputs, no running loop or insufficient capacity leave ARM unavailable with
a reason. Setup errors do not blindly repeat mutations; uncertain operations
are reconciled by generation, epoch and complete configuration before cleanup.
Only matching GUI-owned slots are cleared.

**The hold rule (H2d-2).** Each loop's running inputs are split at connect:
numeric inputs whose descriptor range is finite and includes 0 go to value
groups (full descriptors, an explicit hold of 0, the value the firmware sets
at ARM); every other input (e.g. `position_sp`) goes to a keepalive-only group
whose handle has empty descriptors and sends prefixes only. Nothing is written
at setup; descriptor defaults are never semantic holds. A Busy/Withdrawn
`CMD_CONFIG` (no change, the loop missed its claim window) is retried twice;
the split can need more slots than before, and a shortage leaves ARM
unavailable with the reason. Only Monitor's manual command writes values.
An already-armed connection observes without adopting groups or renewing
another owner's receipts. This includes an armed state seen during slow
discovery: a subsequent watchdog disarm does not authorize takeover. An
explicit DISARM acknowledgement is needed. Boot and link loss invalidate
ownership/traffic; nothing replays values or ARM automatically.

`InstanceTree` exposes stable type/instance keys, loop hierarchy, labels and
health for a configured or pending snapshot. `DescriptorRows` exposes an
instance's discovered names, kinds, types, units, ranges, enums, flags,
parameter slot bits and action arguments/results. Signals/inputs have no
fabricated live values. Editing and subscriptions belong to the screen steps.
`lastError` preserves message, kind, result/detail, family/op, request context,
`outcomeUnknown` and `bootChanged`.

`SafetyKeys` catches Esc at application level, including shortcut override,
focused text fields and popups. It restores the edit before native popup
cancellation, also handling a lost preceding key release. Ctrl+1..4 selects
Device/Layout/Tune/Monitor. Modal scrims begin below the status bar, disable
the underlying page and keep DISARM live. Toasts stack to four; Info/Ok
expire and Fault remains dismissible.

## Connect and Device (H2b-1)

`app/` holds `ConnectPage`, `DevicePage`, `FaultsDrawer` and the shell's
dialogs and overlays; `backend/device_models.*` holds `LoopTable`,
`FaultList`, `RecentDevices` and the readable refusal and reset-reason text.

- **Connect.** Serial ports come from `drivelink::listSerialPorts()`, which
  never opens a port; each row names the device last seen on it. A port held
  by another program shows from the failed open (`lastError.osError`). Baud
  921600/460800/230400/115200; the TCP tab takes any endpoint. Recent devices
  (newest first, eight, keyed by device id) live in `drivelink-gui.ini` in the
  application's config directory (`--settings FILE` elsewhere). Loading shows
  three steps; an incompatible core version shows both versions
  (`Connection::mismatch`) and changes nothing.
- **Device.** Identity and versions, Rename (16 UTF-8 bytes, no control
  characters, `nameProblem()`; it applies live and sets Unsaved, also while
  armed), ARM checks, the faults card, limits (`limits.*` system params) and
  the loop table keyed by loop id. A loop's run time and longest run come from
  one GUI-owned subscription group per running loop (~10 Hz, its two
  statistic signals), configured only on free slots after command setup and
  released by exact match on close; budget is last run time over the period,
  computed on the host. Overruns and CPU per core come from STATUS. A
  `Busy/Withdrawn` SUB_CONFIG (no change by protocol) is retried twice; any
  other refusal leaves that loop's run time empty with the reason as a tooltip.
  After a stalled link, Retry to the same device first clears its own exact
  previous subscription groups.
- **Configuration.** SAVE/REVERT (status bar and Device page), Reboot, Save and
  reboot, Factory reset (destructive dialog with a confirmation box) and
  EXIT_SAFE_MODE (safe-mode banner). The firmware refuses all but Rename while
  armed, so they are disabled with that reason; they are also disabled while a
  requested reboot is in progress. The GUI keeps the changes it made since the
  last SAVE per device id (`unsavedChanges`).
- **Reboots.** A requested reboot never repeats REBOOT. On a link that stays
  open (serial) the GUI waits for Boot and reloads; when the link drops (vdev
  TCP) it reopens the link for up to 30 s. A reboot that never happens ends
  the expectation after the same window. The reboot notice names the cause, the
  reset reason (`esp_reset_reason_t`) and the tracked unsaved changes that were
  lost (none after Save and reboot). An unrequested link loss shows the
  link-lost overlay; Retry reconnects and shows the notice only when the
  device's uptime went back.
- **Faults.** The drawer lists the device's diag entries with the code names,
  severities and `disarms`/`blocks_arm` flags it serves, active first. The
  status-bar badge counts active faults only (STATUS `active_error_count`);
  history stays in the drawer. ACK is disabled with its reason while a
  disarming condition is still active. A refused ARM opens a popover under the
  status bar with the failed checks.

## Firmware update (H2b-2)

`backend/connection_ota.cpp` runs the update on the connection;
`backend/ota_model.*` turns it into the dialog and card data (pure, tested);
`app/OtaDialog.qml` is the G3 "Update firmware" dialog.

- **Choose image.** The Device page's Firmware update card opens a
  QtQuick.Dialogs `FileDialog` (`modality: Qt.NonModal`) only while disarmed;
  the image is checked with `inspectImage` (a complete unsigned ESP32-S3 app
  that fits the slot) and shown with its version, size and SHA-256 before
  Install. "Install without confirming" matches the CLI's `--no-confirm`.
- **Update.** Writing (progress, retried chunks), Verify SHA-256 (FINISH),
  Reboot, Reconnect and check the version, Confirm. REBOOT is sent once
  through the reboot expectation and reconnect window every reboot uses; a
  serial link stays open and waits for Boot, vdev TCP reconnects. After the
  device is back, `confirmOtaCandidate` checks the running build against the
  uploaded image and sends CONFIRM at most once. ARM, Save/Revert, Rename and
  the reboot actions are disabled while it runs; DISARM stays live. Abort is
  offered while writing and verifying (the library ABORTs).
- **Outcomes.** Confirmed; Unconfirmed (deliberately, or no reply settled
  CONFIRM: Confirm or Roll back, never retried automatically); Rolled back
  (the previous build runs again; reset reason when the device knows it);
  Aborted; Failed with recovery text. The reboot notice (cause "Firmware
  update") follows when the dialog closes. "Last update" is kept per device id
  in the settings file.
- **States left by another tool.** The card reads OTA STATUS once per
  connection and boot (again after a failed read). An unconfirmed build shows
  its build id with Confirm (identity-checked only when this window uploaded it)
  and Roll back (one REBOOT); an unfinished or prepared image offers Abort.
  Reboot and Save and reboot are disabled while an image is prepared (REBOOT
  would install it unchecked) or the state could not be read.

## Layout (H2c-1, H2c-2)

`backend/layout_model.*` holds the pure Layout rules (tested without a
device), `LayoutTree` and `ParamTable`; `backend/connection_layout.cpp` the
operations; `app/LayoutPage.qml`, `LayoutDialogs.qml` and `TargetRow.qml` the
G4/G7b screen. Everything comes from the descriptors: loop masks, item flags,
`ref:` categories and the generated `<source>.value` signals. The live half
(H2c-2) is `backend/live_model.*` (pure rules, `ActionTable`),
`backend/connection_live.cpp` and `app/ActionRow.qml`.

- **Tree.** Loops by id (gaps allowed) from both layout views: name, rate, a
  neutral Real-time badge, budget (Device loop statistics), state; modules
  under their loop, indented under the instance wired to them. Pending add,
  remove, label and change are marked. Counts against `limits.loops` and
  `limits.instances`; the Add menu (Instance, Loop) is disabled at a limit.
- **Detail.** Header with Rename (SET_LABEL) and Remove, disabled with the
  reason when something refers to it (wiring, a source, instances in a loop),
  as the device would refuse with `Referenced`. Params grouped by name prefix,
  filters All / Changed / Pending and a name filter. Wiring rows pick instances
  of the param's category; other-loop entries are listed disabled with the
  reason. Source rows pick signals and inputs of the matching value type in
  any loop. Loop detail lists the instances that run in it.
- **Edits.** A typed value is sent on Enter or when the field loses focus,
  only when it differs from the device value and from what was just sent
  (user decision 2026-11-17). Enums, bools and targets send on selection. A
  value that does not parse is refused locally; a device refusal keeps the
  typed value with the reason until it is edited or Esc; clamped values show
  what was applied; an unanswered SET is uncertain and shows the read-back
  value. Every SET and layout op is sent once and followed by
  `refreshConfigAsync`, never retried. Reboot params show current → pending.
  Esc and a DISARM click cancel an open edit without sending (DISARM takes no
  focus; `SafetyKeys.cancelEdit`).
- **Add.** Add instance lists the types with their need (real-time, any,
  not real-time), compatible loops by the pending `realtime` value, the
  device default (lowest compatible id) preselected, and Add loop when none
  fits. Add loop shows the loop type's params with their defaults (core: the
  discovered enum, Any core) and sets only changed values after ADD_INSTANCE.
  Refusals stay in the dialog; nothing is staged.
- **Pending.** The pending banner has Review (every staged change, old → new,
  configured view against pending view), Save and reboot, and Discard all
  (a destructive confirm listing what is dropped). ARM is disabled while a
  reboot is pending. While armed the layout ops, reboot params, wiring and
  sources are disabled with the reason; locked-while-armed params show Locked.
- **Live group (H2c-2).** One GUI-owned subscription group follows the
  selected running node at about 10 Hz (divider `rate/10`): an instance's
  source `.value`/`.age_us` pairs, then its own signals (beyond
  `limits.group_entries` they are counted as not shown); a loop's ticks,
  overruns, last and longest run under the loop's own id. It takes a free
  slot only, after command setup and the Device loop-statistics groups;
  intent is recorded before SUB_CONFIG and a slot is reused or cleared only
  on an exact LIST match, as for loop statistics. One mutation at a time;
  the latest selection wins. It is cleared when the node stops running and
  on close; a setup that fails (no free slot, Busy three times) shows the
  reason and is tried again on the next selection. No new sample for three
  sample periods (at least 1 s) reads "Telemetry unavailable".
- **Live rows.** Signals · live lists the instance's signals (not the
  generated source signals) with value and unit; Statistics · live shows a
  loop's ticks, overruns, runs and budget (last run over the period). Source
  rows show value and age right of the picker and a state line: not
  connected, pending (a staged target is never shown live), missing (the
  producer is not running), no valid sample (age `0xFFFFFFFF` hides the
  value), stale (age above 3 producer-loop periods) or valid.
- **Actions.** Name, "runs in loop" or "paused action", Run. Arguments or a
  blob input open an inline form in the card (no modal; DISARM stays
  reachable), checked against the descriptors before anything is sent; blobs
  are hex, at most 512 bytes. RUN is sent once, one action at a time; no
  reply reads "may have run; not retried", and a configuration refresh
  follows. Blocked with the reason: the instance not running, a pending add
  or remove, a paused action on a real-time loop while armed (loop actions
  stay enabled: user decision 2026-11-17), OTA or a reboot, another action
  running. Results show typed values; a blob out shows size, hex and Copy.
  An action in flight when the link drops raises a "may have run" notice.
- **Custom panel slot.** A second tab, titled by the panel, when
  `DriveLink/Modules/<type name>/Panel.qml` is in the resources
  (`modules/<type name>/`, listed in `modules/CMakeLists.txt`); it is loaded
  with `backend`, `nodeKey` and, if it declares one, `dialogParent`. It takes
  the main column; Wiring, Signals and Actions stay beside it. A panel that
  sets `dialogOpen` dims the page like the shell's dialogs. See "Custom
  panels (H2f)".

## Monitor (H2d-1)

Live plots of any running signal (G5 frames). Pure rules in
`backend/monitor_model.*`, the QML-facing state in `backend/monitor_view.*`
(`connection.monitor`), the groups in `backend/connection_monitor.cpp`, the
plot item in `app/plot_item.*` and the page in `app/MonitorPage.qml`.

- **Signals card.** Per running loop (its statistics) and per running
  instance (signals, and inputs marked "(input)"), from the descriptors; a
  search box; plotted rows show the series colour and the latest value.
  Plot rates per loop: the loop rate divided by 1, 2, 5, 10, … down to about
  1 Hz, default the fastest at or below 100 Hz. The link bar adds every
  subscription group (`9 + 12 + 4k` bytes per sample, this GUI's and other
  clients', from the last LIST) and the status stream to 921 600 baud. An
  add or a faster rate is refused, with the reason and nothing changed, over
  100 %, without a free slot for a new loop, at `limits.group_entries`, at
  16 signals or when a ninth plot would be needed; over budget it offers the
  fastest rate of that loop that fits, applied in one click. A change that
  lowers the load is always accepted. Groups in use and lost frames (counter
  gaps) are shown. A group carries at most 1 kHz: the firmware's link task
  sends each group's latest sample once per 1 ms, so faster loops start at
  a divider that keeps 1 kHz, and a chosen rate follows a live loop-rate
  change down to it.
- **Groups.** One GUI-owned subscription group per plotted loop, refs in plot
  order, with the loop-statistics and live-group rules: free slots only,
  after command setup and loop statistics, intent before SUB_CONFIG, reuse or
  clear only on an exact LIST match, Busy/Withdrawn retried twice, a
  SUB_CONFIG landing on a newer boot releases its slot. One group mutation
  at a time across Layout and Monitor; the latest plot set wins; a failed
  pass retries three times, 2 s apart. Entering the page re-reads LIST.
  Samples are popped every 50 ms into per-signal rings (60 s at the plot
  rate) on device time. Boot clears the data and sets the groups up again;
  a lost link keeps the plot set and rates for the same device; an explicit
  close or another device resets them. The Layout live group exists only
  while Layout is shown, so Monitor can use its slot.
- **Plots.** Stacked, one per unit: a checked signal joins the plot of its
  unit, else the first empty plot, else a new one (at most 8; Add plot and
  remove). Legend chips with the live value and remove; Y axes with round
  ticks; one shared time axis ending at the newest sample. `PlotItem`
  decimates each series to min/max per pixel column on the GUI thread and
  draws scene-graph line strips; Qt's software renderer draws no custom
  geometry, so there it paints a cosmetic 1 px image (offscreen tests and
  screenshots).
- **Toolbar.** Pause/Resume (Space, except while typing or under a shell
  dialog or overlay; Resume starts again from now), Window 2/5/10/30/60 s,
  Cursors (while paused: A–B across every plot with a readout of A, B, Δ and
  Δt), Snapshot CSV (the visible window to
  `Documents/DriveLink/snapshots/<device id>-<time>.csv`, a toast with the
  path, no dialog) and Add plot. Plots and rates are not saved.
- **Plot in Monitor.** Layout's Signals and Statistics cards add the node's
  signals (not its inputs) and open Monitor.

## Manual command (H2d-2)

Monitor's right-hand card (G5 CommandControl) sets inputs by hand. Pure rules
in `backend/command_model.*`, the QML-facing card in `backend/command_view.*`
(`connection.command`), the controls in `style/DLSlider.qml` (Symmetric scale)
and `style/DLCommandControl.qml`.

- **What can be commanded.** "Command another input" lists every running
  input; those outside the hold rule (no finite range, a range without 0, a
  bool or enum, a range finer than 4 decimals) are disabled with the reason.
  The card's inputs are kept for a reconnect of the same device, not saved.
- **When.** Only while this window's ARM was accepted and no status has shown
  the device disarmed since (groups ready, not observing another owner).
  Otherwise the card shows why and every value is 0. DISARM (Esc), a fault,
  the watchdog, Boot, a lost link and close return every value to 0 locally
  at once; DISARM also discards queued values. Nothing replays after a re-ARM.
- **Slider.** Linear from −limit to +limit within the descriptor range; the
  limit starts at 25 % of the descriptor maximum (or of |min| when the
  maximum is 0), can be raised to max(|min|, |max|) with the pencil, and is
  remembered per device and input in the app settings. Values are quantized
  to the shown step (2 decimals below a limit of 10, 1 below 100, whole
  numbers above, more below 0.1); every value sent is the value shown, or 0.
  Only a held handle commands: no keyboard steps.
- **Dead-man and Latch.** Releasing ramps to 0 in 12 steps of 20 ms, each a
  shown value. A held slider that loses its press (focus loss, a page change)
  counts as released. Latch keeps the value on release and on other pages
  until Unlatch (a ramp to 0), Remove or Hold all (0 at once) or DISARM.
  The value field is editable only while latched and sends on Enter; leaving
  it drops the typed text.
- **Frames.** One frame per value group at most every 20 ms carries the shown
  value of each of its inputs (latest wins; a full queue retries on the next
  tick); keepalives run as before.
- **Live param writes.** When Layout writes a live param to an instance with
  inputs in a value group, its queued values are discarded, its inputs go to
  0, unlatch and take no command while the write is in flight; when the write
  ends (reply, refusal or uncertain outcome) its group sends a fresh frame,
  so a mode change resumes from 0 (user decisions 2026-11-18).
- **Not covered.** The dead-man runs on the GUI thread while keepalives run
  on I/O: a frozen GUI with a slider held keeps that value running; DISARM
  (or the watchdog once the app dies) stops it.
- **Tests and tools.** `gui_monitor_tests` `commandLogic` (pure),
  `commandOperations` (vdev), `commandInShell`, and `benchCommand` (env-gated:
  `DRIVELINK_GUI_BENCH_ENDPOINT`, `DRIVELINK_GUI_BENCH_INPUT` as
  `type:instance:item`, `DRIVELINK_GUI_BENCH_VALUE`, optional
  `DRIVELINK_GUI_BENCH_RESPONSE` and `DRIVELINK_GUI_BENCH_MIN_RESPONSE`).
  The app's `--command KEY[@LIMIT][=VALUE[:latch]]` puts inputs on the card
  for screenshots; with a value it arms, so it accepts only a `tcp://` vdev.
  Scenarios `monitor` (one input held, one latched) and
  `monitor-command-disarmed`.

## Tune (H2e-1)

The Tune page (G5) runs test signals on a controller and compares the captured
responses. Pure rules in `backend/tune_model.*`, the QML-facing page in
`backend/tune_view.*` (`connection.tune`), the device side in
`backend/connection_tune.cpp`, the controls in `style/DLGainControl.qml` and
`style/DLSlider.qml` (Log scale). Step metrics come from
`drivelink/tuning.hpp` in libdrivelink.

- **Controllers and gains.** The picker lists running instances, outside the
  loop types, that have a running `testgen` f32 input. Gains are the
  instance's live numeric params flagged `tune` (core v10 `ItemFlag::Tune`);
  tune-flagged enums are combos on top. Gains are grouped by dotted prefix; the
  group whose prefix starts the selected enum value's name is first and open,
  others are collapsed but editable, a group no value selects is open
  ([adding-a-module.md](../../docs/adding-a-module.md)).
- **Gain control.** A log slider over a window of ÷4 to ×4 around the value
  (typed low and high with the pencil, kept per connection), steps ÷2, −10 %,
  +10 %, ×2, arrow keys 1 % (Shift 10 %, Ctrl 0.1 %), values rounded to 3
  significant digits. Gain 0 disables the slider and steps (type a value).
  A drag sends the latest value at most every 50 ms and once on release, never
  two writes of one gain in flight; Enter sends a typed value. An accepted value
  is shown until the device reads it back (or 2 s). Change lines coalesce per
  param; the configuration is read back 300 ms after the last write. Gain writes
  use the Layout live-write path: a commanded instance goes to 0 for the write.
- **Saved values.** Taken while the status shows no unsaved changes (connect,
  SAVE, REVERT, reload); unknown otherwise, which disables "Revert gains to
  saved" with the reason. Revert writes one batch and waits for gain writes
  in flight.
- **Test signal and capture.** Target (a running `testgen` input of the
  controller), waveform (step, ramp, sine, square, chirps), offset, amplitude,
  frequencies (below half the loop rate), duration and repeat, checked before
  Run. The capture defaults to the target and the first same-unit signal of
  the controller at the loop rate (a divider fits it into the capacity), with
  about 6 % before the trigger; the trigger is always the test signal start.
- **Run.** One at a time, only with this window's accepted ARM and its command
  groups ready (F5 or Run; Run shows why it is disabled). The controller's
  manual commands go to 0 for the run (as for a live write); the GUI sends
  CAPTURE CONFIG, ARM and TESTGEN START once. A refused step names itself and
  ABORTs a capture this run configured; no reply to START sends STOP. When the
  signal ends (TestgenDone, or STATUS) the controller's value group sends a
  fresh frame, so a commandable target returns to 0; a target outside the hold
  rule keeps the generator's last value. Stop ends the signal and keeps the
  capture ("stopped"); a disarm ends it with a marker ("cut by a disarm"); the
  capture keeps recording and is read once when Done. A run given up while its
  signal may run (timeout, cleared or unreadable capture) sends STOP once and
  keeps the controller at 0 until STATUS shows the signal stopped; Run is
  blocked meanwhile. If STOP is pending, keep the hold until STOP has landed
  and the device reports the signal stopped. Boot, link loss and close end the
  run without further device calls. The mode, Revert and Restore are locked while a run is active; the
  gains too while Auto re-run is off (with it on a change cancels the run, see
  H2e-2).
- **Runs.** Kept in memory per device (at most 50; the oldest unpinned goes),
  numbered with fixed colours. The overlay shows pinned runs, the newest in
  full and the two before it faded, one plot per unit of the checked signal
  chips. Step runs get rise time (10–90 %), overshoot, settling time (2 %) and
  steady-state error of the response (the first captured same-unit signal);
  other waveforms show "—". A run on irregular device ticks (desktop vdev) says
  so. Row menu: pin or unpin, Export CSV…, remove.
- **Tests and tools.** `gui_tune_tests` `tuneLogic` (pure), `tuneView`,
  `tuneOperations` (vdev: step run, metrics, read-back 0, Stop, refused START,
  a given-up run, disarm mid-run, Revert), `tuneInShell`, and `benchTune`
  (env-gated: `DRIVELINK_GUI_BENCH_ENDPOINT` with `DRIVELINK_GUI_BENCH_STAGE_MOCK=1`
  on a bare board, or `DRIVELINK_GUI_BENCH_TUNE`, `_TARGET`, optional
  `_RESPONSE`, `_STEP`, `_DURATION`, `_MIN_RESPONSE`). The app's `--tune KEY`,
  `--tune-set`, `--signal` and `--tune-runs KEY=V1:V2` build screenshots;
  runs arm, so they accept only a `tcp://` vdev. Scenarios `tune`,
  `tune-running`, `tune-disarm` and `tune-disarmed`.

## Tune session (H2e-2)

Gain history, A/B, Auto re-run and session files. Pure rules in
`backend/tune_session.*` (history, name matching, A/B metrics, the session
file and the run CSV); the state in `backend/tune_view.*`; the cancel path in
`backend/connection_tune.cpp`.

- **Gain history** (left column, under the gains; it scrolls with them at
  1080p). An entry when the controller's gain writes have been quiet for
  0.5 s and the set differs from its last entry; Restore and Revert gains to
  saved make one too (with a note). The first is "loaded from device". A run
  joins the entry of the gains it ran with (a new entry when they changed
  elsewhere). At most 200 per controller. Restore writes the gains that differ
  in one batch (live), only when idle and no gain write is pending; the newest
  entry of this device with the live gains is "current". A set recorded here
  never joins another device's entry.
- **A/B.** The A/B button of an entry with a run marks A, then B (a marked
  entry unmarks; a further pick replaces B). Both marked: only their newest
  runs are plotted, the toolbar says so, and the A/B card replaces Run
  metrics: the gain differences and the equal gains, the four step metrics
  for A and B with B − A, Restore A, Restore B, Exit A/B. The card puts the
  differences beside the table and stacks them below 900 px. A new run, a
  controller change or a removed run ends A/B.
- **Auto re-run** (toolbar chip, on by default; user decisions 2026-11-18). It
  starts a run 0.5 s after the gains are still, when they differ from the
  controller's last run, only after the user's Run under this window's
  current ARM (a DISARM, fault, Boot, link loss or another controller needs a
  new Run) and only while Tune is shown. It uses the same start path as Run.
  It waits for a run or a release in
  progress and drops with the reason when the setup is invalid. Run within
  0.5 s of a change carries it (no second run). With Auto re-run on, the gains
  stay editable during a run and a change cancels it: STOP and ABORT once,
  the run is not kept, the controller stays at 0 until STATUS shows the
  signal stopped, then the re-run follows. Off, the gains lock during a run.
- **Session files.** Save session…, Open… and Export CSV… use non-modal
  native file dialogs, allowed while armed (Esc goes to the dialog; DISARM
  stays clickable). Versioned JSON (`format` `drivelink-tune-session`,
  `version` 1): the device id and name, runs (capture setup, names, units,
  base64 columns of little-endian i64 µs and f64 values, signal, gains with
  names, end, pin, the device each was recorded on), the gain history, typed
  gain windows by controller and gain name, and the controller, test signal
  and capture. Reading checks every field, length, range and time order and
  refuses another format or version. Open (inline confirmation over an
  unsaved session) replaces the runs, history and windows and never writes to
  the device; the setup applies only on the device the session was recorded
  on, and Auto re-run needs a Run again. Strings from a file render as plain
  text.
  Runs and entries of another device are marked and draw no gain ticks;
  their Restore needs a running controller of the same type and label whose
  gains have the same names, and values the gain's type and range hold. Metrics are recomputed on reading.
- **CSV per run.** `#` lines with the run, the controller, the test signal,
  the gains and how it ended, then `time_s` (from the trigger) and a column
  per captured signal, `name (unit)`.
- **Autosave.** While the session is unsaved it is written 1 s after a change
  on a worker thread from a copy (on a device change or close at once) to
  `tune-recovery/<device id>.tune.json` beside the GUI's settings. On the
  next connect to that device the Session card offers Restore it or Discard;
  until then only a recorded run replaces it and other changes are not
  autosaved (the banner says so; Discard autosaves them). Saving and opening a
  session run on the GUI thread (user actions, bounded by the sample cap). Saving, opening or discarding
  waits for a write in flight, then removes it. A session of more than 8 Mi
  samples is neither saved nor autosaved (remove runs); reading refuses it.
- **Tests and tools.** `gui_tune_tests` `tuneSessionLogic` (pure: history,
  matching, A/B, the file round trip and refusals, CSV), `tuneSessionView`
  (fake hooks: entries, Auto re-run under one ARM, cancel, Restore, A/B,
  files, recovery), `tuneAuto` (vdev: no re-run before Run, a re-run, a
  change mid-run cancels and re-runs, Tune hidden, a new ARM); `benchTune`
  adds `DRIVELINK_GUI_BENCH_GAIN` (a tune gain: +10 % re-runs, back during a
  3 s run cancels and re-runs). Screenshot screen and scenario `tune-ab`.

## Plant identification (H2e-3)

The "Plant identification" tab beside "Step runs" (G5) fits a
first-order-plus-dead-time model to one open-loop step and suggests starting
gains:

- **Where the code is.** Pure rules in `backend/identify_model.*` (the naming
  conventions, the step and its capture, the fit of each response, the
  suggestions). The page state is `backend/identify_view.*`
  (`connection.tune.identify`). The run path is in
  `backend/connection_tune.cpp`, the cards in `app/IdentifyCards.qml`. The
  math is `drivelink/tuning.hpp` (`fitFopdt`, `simcPi`).
- **Conventions** ([adding-a-module.md](../../docs/adding-a-module.md), user
  decision 2026-11-18). These are names, not wire data:
  - **PID groups.** A group with `<prefix>.kp` and `<prefix>.ki` (optionally
    `.kd`) is a parallel PID.
  - **Responses.** The controller's f32 signals with a unit whose name a PID
    prefix starts (`velocity` maps to `vel.*`).
  - **Step input.** A commandable `testgen` input, by default the first
    without a unit.
  - **Mode.** The tune-flagged enum value `open_loop` is held during the step.
    The card can pick another value or leave the mode unchanged.
- **1 · Check.** The operator confirms that the actuator can move freely. A
  DISARM, another controller or a new connection clears it, while a tick
  given while disarmed carries into the next ARM. The open-loop caution shows
  as the CheckItem's new Warning state (neutral icon, "warning" tag).
- **2 · Run the step.** The card sets the step input, the mode, "from", "to"
  and the captured length. The signal is one square period: "from" (0
  included) for the captured length as the baseline, then "to" for as long.
  It runs with this window's ARM, the confirmation, and no run, write-back or
  gain or mode write in progress. The sequence:
  1. The controller's manual commands go to 0.
  2. The mode is read from the device. If it differs, the open-loop value is
     written; a refused write runs nothing.
  3. CONFIG, ARM and START are sent once each, as for a run. START is not sent
     if the ARM changed since the click.
  4. When the signal has stopped, whatever ended it (completed, Stop, a
     disarm, a failure), a fresh frame returns the input to 0 while the
     controller stays held. This happens before the capture finishes.
  5. The previous mode is written back. A local block (a configuration action
     running) delays that write; it is never given up. Then the hold ends.
     Boot and close skip write-back. On close or link loss, tell the operator
     the mode may still be set.

  Until the write-back ends:
  - the gains, mode, Restore, Revert, Run, Auto re-run and Open wait, as do
    Save, Revert, reboots, Rename, the firmware update and the Firmware
    card's Confirm and Roll back (a Save would keep the open-loop mode);
  - the tabs stay where they are.

  Identification never counts as the Run that Auto re-run needs.
- **3 · Model and suggested gains.** Every captured response is fitted on a
  worker thread. The fit window ends where the input leaves "to", because
  capture times count samples and the signal's nominal length is not a
  reliable cut on irregular ticks. The response shown first is the settled
  one with the smallest error (velocity over position's ramp). The card
  shows:
  - **The model.** K (response per input unit), T, L, and the fit error
    beyond the noise with good, fair or poor.
  - **Suggestions.** SIMC PI suggestions at Robust, Balanced or Fast:
    `kp` and `ki`, `kd` 0, each rounded to three digits and clamped to its
    range (marked "limited"). There are none for a closed-loop step (the
    mode left or held at a value other than `open_loop`), an unsettled or
    poor fit, a start level still moving, a K ≤ 0, or a response without a
    PID group. K is per unit of the commanded input, so a duty limit below
    "to" in the controller makes K look smaller.

  Apply writes them as one live batch with a gain history entry
  ("suggested (Balanced): K …"). "Back to runs" returns to the Step runs tab.
  The last result stays in memory for its controller (not in session files)
  until the controller, device or connection changes.
- **Tests and tools.** `gui_tune_tests`:
  - `identifyLogic` (pure): conventions, signal, capture, fits including the
    window cut, suggestions and clamps.
  - `identifyView` (fake hooks): checks and confirmation, gating, setup,
    result, plot, Apply, Tune's locks.
  - `identifyOperations` (vdev): the mode switched and written back, the
    input back at 0, the fit, Apply, a pre-level, Stop and disarm mid-step,
    and the mode already open-loop.
  - `benchIdentify` (env-gated: `DRIVELINK_GUI_BENCH_ENDPOINT`,
    `DRIVELINK_GUI_BENCH_IDENTIFY`, optional `_INPUT`, `_FROM`, `_TO`,
    `_DURATION`, `_MIN_GAIN`, `_APPLY`).

  The app's `--identify` option (and the screen `tune-identify`, vdev
  `tcp://` only) arms, runs a step and waits for the fit;
  `--identify-response` picks the response shown. A desktop vdev's irregular
  ticks alias a measured velocity, so the Tune scenarios set the mock
  encoder's velocity filter to 5 Hz.

## Custom panels (H2f)

Module-specific GUI code lives only in `modules/<type name>/`, matched by the
type name the device reports; the rest of the GUI stays generic. A panel's QML
is in the resources; its C++ (typed logic, QML types) is in the
`drivelink_gui_modules` library and registered by `registerModulePanels()`
(`modules/register.cpp`), which the app and the tests call once. A panel uses
the same backend as the generic cards:

- `backend.live`: the selected instance's live signals; each row has `value`
  (text) and `number`.
- `backend.checkAction` / `backend.runAction`: actions by item key (find the
  key by name in `backend.actions`), sent once, never retried;
  `backend.actionResults` (key → `{state, text, values, blobHex, rebooted}`)
  and `backend.actionRunning` follow them. `runAction` emits `changed` before
  it returns: a panel that reacts to `changed` must not handle its own send
  re-entrantly.
- `backend.readTextFile` / `writeTextFile`: a local path or `file://` URL,
  UTF-8, at most 64 KiB (`{ok, text | path, error}`); writes are atomic.
- Its own non-modal native file dialogs (allowed while armed, like Tune's).

The first panel is the `imu.bno055` calibration panel (G7), loaded for
`imu.mock` too: live levels, orientation, guidance, Read and Export, Import
with a review, Apply (write, then apply) with a confirmation, and the
versioned calibration file. Its rules and the file format are in
[modules/imu.bno055/README.md](modules/imu.bno055/README.md).
`gui_panel_tests` (ctest `gui-panels`) covers the generic surface and the
panel in the shell on vdev's `imu.mock`, and the panel's C++ logic. `benchCalibration` (skipped unless `DRIVELINK_GUI_BENCH_ENDPOINT` and
`DRIVELINK_GUI_BENCH_IMU`, type:instance, are set) waits up to
`DRIVELINK_GUI_BENCH_CAL_WAIT` seconds (300) for 3/3/3/3, then reads,
exports, imports and applies through the panel and checks the staged
params; it refuses unsaved changes and REVERTs at the end.

## Settings and About (H2g)

The nav rail's footer item **Settings** (page 4) opens before connecting too:
`DLNavRail` enables its page items (`pagesEnabled`, only with a ready device)
and its footer (`footerEnabled`, whenever no dialog is open) separately. The
page is built by a `Loader` only while shown. It replaces Connect or the
current page; its Close button, the footer item again or another page item
leads back (Close returns to the page it opened from). Leaving Monitor or Tune
for Settings is a page change like any other (dead-man release, Auto re-run
stops); DISARM and Esc are unchanged.

- **Appearance:** Dark, Light or Follow system. `AppSettings` (backend) keeps
  the choice in the GUI's INI file (`appearance/theme`, the recent devices'
  file, so `--settings` isolates it) and resolves "system" from Qt's colour
  scheme; `main.cpp` sets `Theme.dark` from it. Screenshot mode switches
  themes without saving them.
- **Connection (H3b-2):** *Find agents on the network* (`AppSettings::discoverAgents`,
  INI `connection/discoverAgents`, on by default). Off, the Agent tab shows a
  line saying so and never browses.
- **About:** the full version (`cmake/DriveLinkVersion.cmake`, configure
  time), Qt and compiler, the repository `LICENSE` and the third-party
  notices, both compiled into the backend's resources, with Copy.

`gui_settings_tests` (ctest `gui-settings`) covers `AppSettings` and the page
in the shell, disconnected and on vdev. Screenshot scenarios `settings` and
`settings-offline`.

## Windows installer and notices (H2g)

`python tools/package_win.py` (the `installer` check of `validate.py`, and
host-windows CI, which uploads the result) builds the `windows-mingw-release`
preset (Release, no tests, `drivelink-gui` as a Windows GUI program that
attaches to a parent console when started from one), stages the three
programs with `windeployqt` (Basic style only; `platforms/qoffscreen.dll` for
`--out` screenshots), runs a self-contained smoke with Qt and MinGW off PATH,
and compiles `installer/drivelink.iss` with Inno Setup 6 into
`build/installer/DriveLink-<version>-setup.exe`. The AppId in the script is
permanent.

`notices/notices.json` lists every bundled component (version, licence,
copyright, source, licence files, and the DLL names it ships as);
`notices/gen_notices.py --write` renders `THIRD_PARTY_NOTICES.txt` from it, and
`validate.py gen` fails when the file is stale. Staging fails on any DLL that
no component names. A new bundled dependency, Qt module or plugin therefore
needs a manifest entry (and its licence text in `notices/licenses/`) before
the installer builds. Qt's own third-party parts come from the SBOM files of
the Qt install (`C:/Qt/6.12.0/mingw_64/sbom/`), as a superset.

## Linux package (.deb)

`python3 tools/package_linux.py` (the `deb` check of `validate.py`, through WSL
on Windows; CI's linux-package job uploads the result) builds the GUI's
`drivelink` package beside `drivelink-cli` and `drivelink-agent` (H3c,
[host/README.md](../README.md#released-host-packages)). It holds
`/usr/bin/drivelink-gui` (stripped), `/usr/share/applications/drivelink.desktop`
and `/usr/share/drivelink/` (`LICENSE.txt`, `THIRD_PARTY_NOTICES.txt`; not under
`/usr/share/doc`, which minimised systems drop) with
`/usr/share/doc/drivelink/copyright` pointing there, and Depends
`drivelink-cli` at its own version (the CLI and vdev moved there). It is
`build/deb/drivelink_<version>~ubuntu<release>_<arch>.deb`, built only where
Ubuntu's Qt meets the 6.10 floor (26.04). It bundles nothing: Qt and the C++
runtime come from Ubuntu's packages, so staging fails on any shared library.
Depends is `dpkg-shlibdeps`' list plus `QML_PACKAGES`, the runtime QML
modules; a new `import Qt…` in the GUI needs its `qml6-module-*` package there
(`tools/tests/test_package_linux.py` checks it). CI installs it with
`drivelink-cli` in a clean container, amd64 and arm64, and renders every screen
(`host/test/installed_gui_smoke.py --deb`).

## Remote mode (H3b-1)

See the [remote connection walkthrough](https://taruntom1.github.io/drivelink_v2/pi/connect/) and [control handoff procedure](https://taruntom1.github.io/drivelink_v2/pi/control/).

A connection through `drivelink-agent` ([agent.md](../../docs/protocol/agent.md),
agent protocol v3) is the same `LocalConnection` on a TCP link that sent the
agent Hello (`client_kind` Gui, the name `drivelink-gui@<host>`, `--client-name`
to change it): Layout, Monitor, Tune and OTA run through the agent unchanged.
`backend/connection_remote.cpp` adds the lease, the agent's events and the
client list; `backend/remote_model.*` holds the pure rules (tested in
`gui-remote`). Run `drivelink-gui --agent host[:port] --device ALIAS --token T`
(or `DRIVELINK_AGENT_TOKEN`), or use Connect's Agent tab.

- **Agent tab.** A typed address (`host` or `host:port`, port 47620 by default;
  a host name resolves through `resolveTcpEndpoint`) and token; *List devices*
  opens a lobby (`listAgentDevices`) and shows the agent's device list (alias,
  name, id, online, clients, lease holder); *Connect* names one device. The
  list is not in the G6 frames; it is built from the G2 components
  ([gui-design.md](../../plans/gui-design.md)). Refusals show as unreachable,
  version mismatch, bad token, unknown device or no device (offline). Recent
  agents (address, device, last device id and name) live in the GUI's INI
  file (`recentAgents`); the token is stored there only when *Remember token*
  is checked (it is by default; user decision 2026-11-19), and never
  otherwise. Unchecking it or forgetting the agent removes a stored token.
- **Found on the network (H3b-2).** While the Agent tab is shown and
  discovery is on, the backend browses for agents over mDNS
  (`discovery::browseAgents` on a worker thread, `agentBrowse`: state off,
  searching or idle, and the agents): rounds at 0, 1, 3, 7, 15 and 31 s, then
  every 60 s; *Search again* restarts the back-off; an agent missed in two
  rounds in a row is dropped (`FoundAgents`). An agent on another agent
  protocol shows *Incompatible* and cannot be picked. Picking one fills the
  address (clearing a token typed for another address), takes the token of a
  recent agent with that address, and lists the devices when there is a token.
  `DRIVELINK_GUI_FAKE_AGENTS` (a JSON array) replaces the network for
  screenshots (`remote-agent-found`, `remote-agent-found-empty`). A GUI on the
  same PC cannot find an agent in mirrored WSL ([bench.md](../../docs/bench.md)).
- **Observer first.** The connection starts with the holder named by the
  Hello's device record and follows the agent's Lease events (the truth: a
  Take that timed out may still be granted later). Command groups are set up
  only while this window holds the lease; ARM, manual commands and Tune runs
  say "needs the control lease" otherwise. DISARM, ACK, reads, live params and
  Monitor/Layout subscriptions stay open to an observer. Take preempts another
  holder after the operator confirms it is safe to stop it (unchecked box);
  with nobody holding it, Take goes at once. A grant leaves the device
  disarmed (the agent disarmed it for the handoff) and never arms. ARM answered
  `Busy/None` right after a handoff (the previous holder's command group is
  still being cleared) is reported, not repeated.
- **Losing the lease.** On a release or a revocation (a Lease event naming
  this window as `previous_id`) the agent has already stopped this window's
  command frames, disarmed the device and cleared its command groups: the GUI
  forgets them without CMD_CLEAR, cancels an ARM in flight, returns manual
  values to 0, and a run ends on the disarm as on any DISARM (Auto re-run
  needs a new Run). A revocation shows the Revoked chip, a banner and the
  *Control lease revoked* dialog until acknowledged.
- **Exclusive operations** (SAVE, REVERT, reboots, factory reset, safe-mode
  exit, rename, layout ops, actions including calibration, OTA) follow the
  agent's rule: this window holds the lease, or nobody does and the device is
  not armed (`exclusiveBlocked()`); otherwise they are disabled with the
  holder's name.
- **Live edits while another client holds the lease** (G6): a param write from
  Layout or Tune waits unsent in `liveEdit` until *Apply live* (or Cancel).
  The confirmed param keeps going without a question while its edit continues
  (writes at most 1.5 s apart, a drag); *Don't ask again for this connection*
  lasts until the next connection. While a question is open another edit is
  dropped unsent. The holder is never asked.
- **Lease chip and banners.** The status bar's `DLLeaseChip` (Available, Mine,
  ROS, Other, Requesting, Revoked; a holder whose `client_kind` is Ros is ROS)
  leads to Device > Clients and lease. An observer sees "driven by <holder>"
  or "ARM needs lease" beside the arm state, and a ROS holder raises the
  *ROS is driving* banner with *Take lease…*.
- **Device page.** Agent and device, Remote access, Clients and lease
  (ClientList, read at connect, on every Lease event and every 2 s while
  Device is shown; Take, Release, Refresh) and Recent agent history.
- **History.** The History events sent right after the Hello (read from the
  Session's event queue, which a remote connection drains for agent events)
  are shown newest first in the Device card and the faults drawer, labelled
  Agent history beside This connection's entries; diag entries are named from
  the device's code table.
- **Device loss.** The agent's Closing event names the cause on the link-lost
  overlay; Retry reconnects through the agent (a device still offline shows
  "no device"). Through an agent a vdev reboot is a device loss (vdev drops
  its TCP link); a serial device keeps its port and its Boot reloads in place.

`gui_remote_tests` (ctest `gui-remote`): the pure rules on both OSes; on Linux
also through `drivelink-agent` and vdev (observer, take/arm/release, revocation
and DISARM from an observer, exclusive operations, live-edit confirmation,
history, client list, refusals, device loss).

## Theme

Figma's variables are the source of truth. `tokens.json` holds every variable
of the Primitives, Color (Dark, Light), Size and Type collections, the text
styles and the effect styles. `gen_theme.py` writes the `Theme` singleton:

- each Color variable as a `color`, `Theme.dark` choosing Dark or Light
  (primitives are resolved, not exposed);
- Size and Type as numbers and strings (`Theme.space8`, `Theme.sizeControl`,
  `Theme.typeFamilyUi`);
- each text style as `Theme.font<Style>` plus `Theme.line<Style>`
  (`Theme.fontBodyNumeric`, `Theme.lineBodyNumeric`); styles whose Figma
  description mentions `tnum` enable tabular numerals;
- each effect style as colour, blur, offset and spread
  (`Theme.elevationDialogBlur`).

The QML name is the one in the variable's description (`QML: Theme.bgApp`),
else the path in camel case. Two Figma variables are both `Theme.borderFocus`;
the Size one is renamed `borderFocusWidth` in the generator's reviewed rename
table, and any other clash fails the generator.

`Theme.qml` and `tokens.json` are committed; never edit them by hand.
`validate.py gen` fails when either is stale. After a token change in Figma:

1. Run `theme/export_tokens.js` through the Figma MCP `use_figma` tool against
   file `49NRHfKe8rOoVlloLLCqLh` (read-only) and save the returned JSON.
2. `python host/gui/theme/gen_theme.py --import exported.json` rewrites
   `tokens.json` in its canonical form and `Theme.qml`.
3. Review both diffs and the gallery.

## Style

`DriveLink.Style` maps each G2 component to one control, built on
`QtQuick.Templates` with every value from `Theme`. Call
`drivelink::gui::installStyle(engine)` (`style/style.hpp`) before loading QML:
it registers the bundled fonts and the `dlicon` image provider.

| Figma | QML | Notes |
|---|---|---|
| Button | `DLButton` | `kind`: Primary, Secondary, Ghost, Danger; `iconName`; 40 % opacity when disabled |
| IconButton | `DLIconButton` | `size` Md 32 / Sm 24; the tooltip is the accessible name |
| ArmButton | `DLArmButton` | `mode` Arm, Arming, Disarm, AckFault; disabled Arm is "Arm disabled", disabled Disarm "Disarm disabled"; emits `armRequested`, `disarmRequested`, `ackFaultRequested` |
| StateChip | `DLStateChip` | `status` Offline … SafeMode; Fault and Safe mode emit `faultsRequested` |
| Badge | `DLBadge` | 8 tones, Md 24 / Sm 20, optional 12 px icon |
| DeviceChip | `DLDeviceChip` | name, id (12 hex digits shown with colons; shown as the name when unnamed), connection |
| NavItem, NavRail | `DLNavItem`, `DLNavRail` | controlled: `activated(index)`, the shell sets `currentIndex` |
| StatusBar | `DLStatusBar` | `mode` NoDevice … LinkLost; centre stays centred; DISARM enabled whenever there is a link |
| Banner | `DLBanner` | 4 tones, up to 3 actions (Secondary, Primary, Secondary) |
| Dialog | `DLDialog` | Default / `destructive`; optional confirmation checkbox; Enter accepts only a Default dialog; `closeOnAccept: false` emits `acceptRequested` and stays open until the owner closes it |
| Toast | `DLToast` | Info and Ok dismiss after 4 s; Fault stays |
| Tooltip | `DLToolTip` | 500 ms delay, wraps at 320 px |
| Icon/* | `DLIcon` | a bundled Lucide icon by name, recoloured |
| SpinBox | `DLNumberField` | a TextField (QML SpinBox is integer-only); `status` Normal, Edited, Pending, Clamped, Rejected, Locked; unit inside |
| Switch | `DLSwitch` | optional label |
| TabButton | `DLTabButton` | accent underline when checked |
| Menu, MenuItem | `DLMenu`, `DLMenuItem` | not modal; MenuItem `detail` second line |
| TreeItem | `DLTreeItem` | loop header (52 px, rate, Real-time badge, budget, state) or module row by `depth`; pending, failed, offline |
| ParamRow | `DLParamRow` | editor by `editor` (number, gpio, enum, bool); commit rules above; Unsaved/Pending bar and note |
| RefPicker, SourcePicker | `DLRefPicker` | `[chip] › target`; candidates with detail or disabled reason; `status` Pending, Rejected, Unset |
| AddMenu, filter chips | `DLButton` + `DLMenu`, `DLFilterChip` | the Instances card's Add; All / Changed / Pending |

Helpers: `DLShadow` draws the elevation styles without shader effects;
`DLReasonHover` wraps a control and explains why it is disabled (a disabled
item receives no hover).

Rules the controls and shell keep:

- The controls draw and emit requests; they never talk to a device.
- `DLDialog` is not a Qt modal (that would block the status bar); the shell
  dims and blocks only the page under the status bar, so DISARM stays live.
- The application-wide Esc → DISARM shortcut, also inside text fields, belongs
  to the shell, not to `DLArmButton`.
- Hover cannot be forced offscreen; pressed states set `down`.

## Gallery and tests

```bash
drivelink-style-gallery                   # interactive
drivelink-style-gallery --out shots/      # gallery-dark.png, gallery-light.png
ctest --preset windows-mingw -R "^gui-"   # or the linux preset
```

With `--out` the gallery renders offscreen with the software scene graph
(override with `QT_QPA_PLATFORM` / `QT_QUICK_BACKEND`). The tests run the same
way on both OSes. Qt warns once that it found no system font directory
offscreen on Windows; the bundled fonts are registered explicitly.
Wait for `waitForItemPolished` before clicking inside a freshly opened popup:
positioners lay out on polish, and a click computed earlier lands elsewhere.

`gui-backend` covers command planning, Qt model structure, lifecycle/thread
delivery, multi-loop keepalives without value frames, arming/refusals, fault
acknowledgement, delayed/lost replies, early and repeated DISARM, uncertain
ARM close, observer ownership, Boot, link loss, edit/popup cancellation, scrim
access, keys and toasts. Linux also exercises Boot in place over a PTY.
H2b-1 adds device text/models, recent devices, protocol mismatch (a fake
older-version TCP device), rename/SAVE/REVERT/reboot and the notice, Save and
reboot, factory reset, safe-mode exit, free-slot subscriptions, the reconnect
window and Retry after a stall. H2b-2 adds the OTA view models and, against
vdev, a confirmed update, install without confirming then Confirm, roll back,
abort while writing (DISARM live), a lost REBOOT that is not repeated (vdev
`drop-reboot`), states left by another client, disconnect while reconnecting,
the dialog in the shell and (Linux PTY) an update with Boot in place.
H2c-1 adds the pure Layout rules and models (`layoutLogic`) and, against vdev,
Add loop/instance, refused adds, wiring, a source and a BadRef, Remove refused
locally, labels, clamped/applied/pending params, Discard all, the armed lock
and Save and reboot (`layoutOperations`), plus a DISARM click that keeps an
open edit unsent; `tst_layout.qml` covers the param row's commit rules, the
tree item and the picker.
H2c-2 adds the pure live rules (`liveLogic`: plans, every source state,
statistics, argument and hex parsing, block rules) and, against vdev
(`liveDataAndActions`), selection values, slot reuse, a stale source (vdev
`loop-stall`), typed and blob results, local refusals with nothing sent, the
armed rule, a lost RUN reply sent once (vdev `drop-action-reply`), Boot
notifying the live view, and clearing; `layoutLiveInShell` covers the cards,
the panel tab and the action form in the shell.

H2d-1's `gui_monitor_tests` (ctest `gui-monitor`) cover the pure Monitor
rules (`monitorLogic`, `plotLogic`: catalogue, rates and the 1 kHz cap,
budget, refusals and suggestions, plans, rings, decimation, axes, cursors,
loss, CSV), the view without a device (`monitorView`), groups on vdev
(`monitorOperations`: free slots, reconfigure, a refused add without a free
slot, the Layout live group released and restored, Boot, close clearing
only what it owns), the page in the shell (`monitorInShell`: Plot in
Monitor, drawing, Space, cursors, snapshot) and a 16-series plot frame
(`plotPerformance`). Shared helpers are in `tests/test_support.hpp`.
`benchMonitor` (skipped unless `DRIVELINK_GUI_BENCH_ENDPOINT` is set) plots
the fastest loop's statistics at its full rate, then beside another 1 kHz
group, then with the loop at 2 kHz live, and compares the measured rate,
ticks per sample and lost frames with the prediction; it restores the
board on every exit.

`benchOta` is the bare-board OTA check, skipped unless
`DRIVELINK_GUI_BENCH_ENDPOINT` and `DRIVELINK_GUI_BENCH_OTA_IMAGE` are set: it
installs and confirms the image through the backend and, with
`DRIVELINK_GUI_BENCH_OTA_RESTORE`, installs that image (the board's original
app) afterwards. It refuses to start with unsaved or pending changes.

`benchBoard` is the bare-board check, skipped unless
`DRIVELINK_GUI_BENCH_ENDPOINT` names a device (`serial://COM3`, or a provisioned
vdev for a dry run). On the normal firmware it renames, saves and reboots,
arms and disarms, raises and clears a real disarming fault through a temporary
`actuator.mock` (`diagnostic` action) and acknowledges it, then restores the
layout and the original name. It refuses to start with unsaved or pending
changes.
Native arming fixtures SAVE their 10 Hz loop rates and 1 s watchdog so a
reboot retains the desktop scheduling allowance.

## Shell screenshots

The app's `--out DIR` writes four PNGs (2560×1440 and 1920×1080, Dark and
Light), waits for live state with a timeout, then disconnects. `--screen`
accepts `connect`, `device`, `layout`, `tune`, `monitor`, `drawer`, `dialog`,
`rename`, `reset`, `unsaved`, `notice`, `layout-review`, `layout-add-instance`,
`layout-add-loop` (with `--select TYPE:INSTANCE` for the Layout screens),
`monitor-paused`, `monitor-budget` (with `--plot KEY,...`, `--rate
LOOP:DIVIDER,...` and `--window S` for the Monitor screens) and, with `--image FILE`, `update` (the
image choice), `update-writing`, `update-confirmed`, `update-rolledback`;
`--expect-state` accepts `ready`,
`fault`, `safe`, `lost`. Except for Connect, pass an endpoint. `--settle MS`
waits before capturing (loop run time needs about a second); `--settings FILE`
isolates recent devices. It does not inject faults or provision a supplied
device; `unsaved` and `notice` rename (and reboot) the device, so they accept
only a `tcp://` vdev endpoint, as do the `update-*` screens (they install
firmware).

Remote screens (H3b-1) go through `drivelink-agent`: `--agent HOST:PORT
--token T --device ALIAS` (`--client-name`, `--take-lease`) with `device`,
`tune`, `monitor` or `drawer`, and `agent-connect` (the Agent tab's device
listing; `--agent`/`--token` optional), `agent-failure` (the failed connect),
`lease-take`, `lease-release`, `revoked` (these sit on Device, or on Tune with
`--tune`) and `live-edit` (`--live-edit KEY=VALUE`, on Tune with `--tune`,
else Layout). With `--take-lease` the app prints `GUI LEASE MINE` once the
lease is this window's so a script can sequence another client. The Linux-only
`host/gui/tests/remote_scenarios.py` (not a ctest) provisions one sample vdev,
starts the agent and raw clients that play ROS or a second GUI
(`agent_integration.RawClient`) and renders sixteen G6 states into
`OUT/<scenario>/`; in WSL:
`python3 host/gui/tests/remote_scenarios.py --gui $B/gui/app/drivelink-gui --vdev $B/vdev/drivelink-vdev --agent $B/agent/drivelink-agent --out DIR [--only NAME...]`.

From the repo root, the isolated Windows vdev campaign is:

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.12.0\mingw_64\bin;' + $env:PATH
python host/gui/tests/scenarios.py --gui host/superbuild/build/windows-mingw/gui/drivelink-gui.exe --vdev host/superbuild/build/windows-mingw/vdev/drivelink-vdev.exe --backend host/superbuild/build/windows-mingw/gui/gui_backend_tests.exe --out build/gui-scenarios
```

The script defaults to offscreen/software rendering, runs the backend checks
when `--backend` is supplied, and renders twenty-eight scenarios, the last four the firmware update screens (it
writes a non-executable vdev image for them), into separate folders (112 PNGs,
dimensions checked). The Layout and Monitor scenarios first build the G7b three-loop
sample on vdev with `drivelink-cli` (next to vdev, or `--cli`), save and
reboot it, and stage changes for the pending ones; `--only NAME...` picks some. `--smoke`
renders only Connect and Device;
CTest uses that subset on both OSes. Use the CTest-resolved executable paths
on Linux (`ctest --preset linux -N -V -R '^gui-'` from `host/superbuild`).
