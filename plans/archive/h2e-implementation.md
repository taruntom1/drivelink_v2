# H2e Tune: plan and results

Brief: [host-steps.md](../host-steps.md) "H2e Tune". Design: G5 Tune frames (run
results, disarmed, run in progress, stopped by a disarm, A/B compare, plant
identification) and the G2 GainControl and Slider `Scale=Log`
([gui-design.md](../gui-design.md), node ids in
[gui-design-status.md](gui-design-status.md)).

## User decisions (2026-11-18)

| Topic | Decision |
|---|---|
| Split | Three PRs, one session each, each with an independent Opus review at high effort: **H2e-1 Runs** (this record's first part), **H2e-2 Session** (gain history and restore, A/B compare and its 1080p card, Auto re-run, session files, CSV export; how Open/Save pick a file, since a native dialog cannot pass Esc to DISARM), **H2e-3 Plant identification** (FOPDT fit and suggested gains in libdrivelink, the tab, CheckItem warning; how suggested gains map onto tune-flagged params generically). |
| Gains | Marked in the definitions by a descriptor flag, not by names. All eight `ItemFlag` bits were in use, so GET_ITEMS item `flags` widens to u16 with `Tune = 0x0100`: **core v10**, firmware, host, vdev and docs in H2e-1. |
| Mode groups | Flag and prefix order: tune-flagged enums are combos on top; tune-flagged numerics group by dotted prefix; the group whose prefix starts the selected enum value's name is first and open; a group no value selects is open; the others collapsed but editable. Documented in [adding-a-module.md](../../docs/adding-a-module.md). |
| Run end | Return to 0: during a run the controller instance's manual commands are suspended at 0 (the H2d-2 live-write mechanism); when the run ends (completed, Stop, disarm) a fresh frame returns a commandable target to 0. A target outside the hold rule (`position_sp`) keeps the generator's last value; the run card says so. |
| Bench board | Flash the verified v10 normal firmware and leave it installed. |

## H2e-1 scope

- **Wire.** `defs::ItemFlag` is u16 (`Tune = 0x0100`), `ItemDesc::flags` u16,
  GET_ITEMS entry `flags u16` (item size 31), core `kVersion = 10`. Generator
  key `tune: true`, only on live (not `reboot_required` / `read_only`) numeric
  or enum params. `controller.cascade_pid`: `mode` and the `vel.*` / `pos.*`
  gains and d-cutoffs carry it. libdrivelink reads u16; the CLI prints the
  wider number; the Python test peers use `CORE_PROTOCOL_VERSION`.
- **libdrivelink.** `drivelink/tuning.hpp` `stepMetrics`: rise 10–90 %,
  overshoot, settling (2 % band), steady-state error (mean of the last 10 %),
  each optional with a reason.
- **GUI.** `backend/tune_model.*` (pure rules), `backend/tune_view.*`
  (`connection.tune`), `backend/connection_tune.cpp` (gain writes and the run),
  `PlotItem` on a `PlotSource` interface (Monitor and Tune), `DLSlider` Log
  scale with ticks and labels, `DLGainControl`, `TunePage.qml`, screenshot
  options and four scenarios. Details in [host/gui/README.md](../../host/gui/README.md) "Tune (H2e-1)".

## Review

Independent Opus review at high effort (wire change; runs start test signals
that move motors), then a focused re-check of the fixes. Every finding held
and was fixed:

| Finding | Fix |
|---|---|
| A given-up run (timeout, cleared or unreadable capture) queued STOP and ended the hold at once: the zero frame could land before STOP and be discarded, leaving the target at the generator's last value while the GUI showed 0 | The hold moves to a release that ends only when testgen STATUS shows the signal not running; Run is blocked meanwhile; a timeout also ABORTs the capture. vdev test (capture cleared mid-run); with the old release the target stayed at 2 rad/s |
| `integration.py` sent a v9 REBOOT header (its check passed for the wrong reason); `cli_replay.py` likewise | Both use `CORE_PROTOCOL_VERSION`; replay packs u16 flags |
| Boot ended the run with device calls before the snapshot swap | `TuneEnd::DeviceGone`: no STOP and no `endWrite` on Boot or close |
| A CONFIG/ARM without a reply was reported as a START failure and left the capture armed | The chain reports the failing step and ABORTs a capture after a CONFIG that may have been accepted (never after a refused CONFIG: it may be another client's) |
| Revert could put a second write of a gain in flight, matched gains by item only and recorded the dragged value | Revert waits for gain writes, matches the full ref on the current controller and records the reverted value |
| Mode and gains editable during a run | Locked while a run is active (QML and the view's entry points) |
| Tune change lines kept after SAVE | Dropped with the change list |
| Re-check: a definite STATUS refusal kept the release forever | A definite refusal ends the hold (no generator runs on that loop) |
| Re-check: Run could start mid-drag or with a gain write pending | Run waits for gain writes and drags |

Also found by the first full gate: Tune's snapshot fingerprint skipped a new
device's snapshot (cleared on a device change).

## Results

- **Gate.** `python tools/validate.py full` passed (gen, native, host-win,
  host-linux, fw, fw-nolog, fw-hil-app). The first run caught the CLI replay
  peer still speaking v9 and the Tune snapshot fingerprint skipping a new device.
- **vdev.** `gui_tune_tests`: `tuneLogic`, `tuneView`, `tuneOperations` (step
  run with metrics, rise 10–90 % between 0.01 and 1 s; velocity_sp back at 0;
  a latched duty_sp zeroed for the run and freed after; Stop at about 0.5 s; a
  given-up run held until STOP; refused START aborts the capture; disarm
  marker; Revert), `tuneInShell`; `gui_quick_tests` 62, `gui_backend_tests` 38,
  `gui_monitor_tests` 11, host tests (`test_tuning`, the u16 flag). Scenarios
  `tune`, `tune-running`, `tune-disarm`, `tune-disarmed` at both sizes and themes.
- **Bare board** (COM3, v10 `59429185fa2d504a`): hardware, identity, testgen
  (mock) and capture smokes, `benchBoard` and `benchTune` with a staged mock
  passed; details in [hardware-log.md](../hardware-log.md). The mock run shows no
  metrics by design: its target is the unitless `duty` input and the only
  other captured signal is `velocity` (rad/s), so there is no same-unit
  response; the controller mode (velocity_sp against velocity) was dry-run on
  vdev (rise 88 ms). An unrecorded motor layout
  was removed at the user's request.
- **Deferred.** `benchTune` with a real motor (row in the status file).
- **Observed.** A long capture READ blocks the serial service executor while it
  reads; desktop vdev ticks irregularly, so its runs show "irregular timing".

## H2e-2 Session

### User decisions (2026-11-18)

| Topic | Decision |
|---|---|
| Gain change during a run | With Auto re-run on, the gains stay editable during a run and a change cancels it: STOP and ABORT once, the run is not kept, the gain applies live, the controller stays at 0 until STATUS shows the signal stopped (the H2e-1 release), then the re-run follows. With it off the gains lock during a run (H2e-1). Mode, Restore and Revert stay locked during any run. |
| Auto re-run permission | Resets on every ARM: a run starts by itself only after the user's Run under this window's current ARM; DISARM, a fault, Boot, link loss or a controller change need a new Run. |
| File dialogs | Non-modal native dialogs for Save session, Open and Export CSV, allowed while armed (Esc goes to the dialog while it is open; DISARM stays clickable). |
| Open | Replaces the runs, history, windows and setup; never writes to the device. Another device's runs and entries are marked; Restore matches by controller type and label and gain names. |
| History entries | When the gain writes have been quiet 0.5 s and the set differs from the controller's last entry; Restore and Revert make entries; first "loaded from device"; 200 per controller. |
| Unsaved sessions | Autosaved per device id beside the settings and offered on the next connect; recording a run replaces the offer. |
| Hardware | vdev only; the H2e-1 deferred motor row gains the Auto re-run checks (`DRIVELINK_GUI_BENCH_GAIN`). |

### Scope

- **Pure.** `backend/tune_session.*`: history rules (`recordEntry`,
  `gainChanges`, `equalGains`, `entryRun`), `resolveGains` (by names),
  `compareMetrics`, `writeSession`/`readSession` (format
  `drivelink-tune-session` version 1) and `runCsv`.
- **View.** `TuneView` gains `history` (`HistoryRows`), `compare`,
  `autoRerun`, `session`, `recovery`, Restore, A/B, the settle timer, Auto
  re-run, files and autosave. `RunRecord` carries the controller's type and
  label, gain names and the device it was recorded on.
- **Connection.** `cancelTuneRun` (a cancel before START's reply waits for
  it), `m_armEpoch`, hooks `cancel`, `armEpoch`, `autoBlocked` (Tune shown).
- **QML.** Gain history card, the Auto re-run chip, the A/B card, the Session
  card (recovery, inline Open confirmation), run menu Export CSV…, three
  non-modal `FileDialog`s. Screenshot screen `tune-ab`.

### Review

Independent Opus review at high effort (Auto re-run and the cancel path start
and stop test signals; session files are parsed from disk), then a focused
re-check of the fixes. The cancel path and the Auto re-run gating held; the
findings and their fixes:

| Finding | Fix |
|---|---|
| Any small change (a gain tweak, a window, a pin) autosaved over an offered recovery one second later | While an offer shows, only a recorded run replaces it; other changes wait (the banner says so) and are autosaved after Discard |
| Opening a session (or restoring a recovery) kept Auto re-run's permission, so a gain change could start a signal the user had not run under this ARM | Open and recovery need a Run again |
| After an Open the newest entry showed "current" with gains that were not live; local sets could join another device's entry | "Current" is the newest entry of this device with the live gains; entries are per device; Open records the live gains when they differ |
| Restore from an edited file could write an infinite f32 or an out-of-range integer | `resolveGains` refuses values the gain's type cannot hold, and fractions for integers |
| The whole session was serialized on the GUI thread for every autosave (a large one freezes DISARM's thread) | Autosave writes on a worker from a copy; Save, Open and Discard wait for it before removing the file; sessions above 8 Mi samples are not saved |
| Run completions matched only the boot generation | A per-connection run serial |
| Setup setters relied on QML for the run lock | Backend guards while a run is active |
| Open asked only over unsaved runs; file strings could render as rich text; Restore A/B gave no reason | Asks over any unsaved session; plain text; reasons on the buttons |

Accepted, not changed: Save and Open parse on the GUI thread (user actions,
bounded by the sample cap); the per-row "current" scan (200 entries at most).

### Results

- **Gate.** `python tools/validate.py full` passed (gen, native, host-win,
  host-linux, fw, fw-nolog, fw-hil-app). An earlier full run (before the
  review fixes) failed once in the firmware native test "Configurable loops:
  pending compatibility" (`revert()` after `start()`); it passed alone and in
  the final run (no firmware change in this step).
- **vdev.** `gui_tune_tests`: `tuneSessionLogic` (history, matching and type
  ranges, A/B, the file round trip and refusals, CSV), `tuneSessionView`
  (entries, Auto re-run under one ARM, Run within 0.5 s, a waiting re-run,
  cancel, Restore, A/B, Current, files, recovery and its offer, Open resets
  the permission), `tuneAuto` (no re-run before Run, a re-run, a cancel
  mid-run and while START is in flight, Tune hidden, a new ARM, autosave on
  close), and the H2e-1 tests (`tuneOperations` with Auto re-run off).
  Scenarios `tune` and `tune-ab` at both sizes and themes.
- **Bench dry run.** `benchTune` with `DRIVELINK_GUI_BENCH_GAIN` on the sample
  vdev passed (row in [hardware-log.md](../hardware-log.md)). Real motor deferred
  (the H2e-1/H2e-2 row in the status file).

## H2e-3 Plant identification

### User decisions (2026-11-18)

| Topic | Decision |
|---|---|
| Mode | The step switches the controller's tune-flagged mode to `open_loop` and writes the previous value back afterwards (whatever ends the step), not the user. |
| Mapping | Naming conventions, no wire change: `<prefix>.kp/.ki/.kd` PID groups, responses named after the prefix, the `open_loop` value, a commandable unitless step input ([adding-a-module.md](../../docs/adding-a-module.md)). |
| Gains | SIMC PI with Robust / Balanced / Fast (τc 3L / L / L/2, at least four loop periods); kd suggested 0. |
| Duty from | A pre-level. Built as one square period of the test generator (first half "from", second half "to"), not a manual command, so no new command source and the hold rule unchanged (approved with the plan). |
| Models | Plan, backend, tests and review fixes on Opus 5.5 (high); the QML cards, the CheckItem Warning state and the screenshot option on a Sonnet 5.5 subagent (high) from a written contract, reviewed by the session; independent review on Opus (high). |

### Scope

- **libdrivelink.** `fitFopdt` (step found in the input; levels from 20 % windows; two-point start, grid over L, golden section on log T, K by least squares; noise-aware `settled`, `steady_start`, error beyond the noise; SNR on the standard error of the two levels), `fopdtValue`, `fitQuality`, `simcPi`.
- **GUI.** `backend/identify_model.*` (conventions, signal, capture, fits, suggestions), `backend/identify_view.*` (`connection.tune.identify`, the fit on a worker), the run path in `connection_tune.cpp` (mode read from the device, switch, write-back after the signal stops with the hold kept, waits out local blocks), Tune's locks (`TuneView::setIdentifying`, `busy()`), config actions blocked meanwhile, `app/IdentifyCards.qml`, the tabs, `DLCheckItem` Warning, `--identify` and the `tune-identify` scenario.

### Internal choices

- The step is always a square period: from 0 too, because a capture armed just before START may hold no samples before the trigger, and the first half is the baseline.
- The fit window ends where the input leaves "to": capture times count samples, so the signal's nominal length is not a cut on irregular ticks.
- The confirmation is cleared by a DISARM (a tick given while disarmed carries into the next ARM).
- Results stay in memory for their controller, not in session files (format v1 unchanged).
- The default response is the settled one with the smallest error; suggestions need an open-loop step.
- A change line back at its starting value is dropped (gains too).

### Review

Independent Opus review at high effort (identification starts a signal that moves a motor and writes the mode). Findings and fixes:

| Finding | Fix |
|---|---|
| Gains could be suggested from a closed-loop step (velocity_sp with the mode left at velocity) and applied | Suggestions need an open-loop step (`open_loop` held or already set; no tune enum and a unitless input) |
| The write-back gave up on a local block (a rename running), and during a disarm-cut capture Save could persist `open_loop` | The write-back waits and retries on the timer; SAVE, REVERT, reboots, rename and OTA are blocked until it ends |
| The write-back waited for the whole capture | It starts when the signal has stopped |
| The hold ended before the write-back (the Command card live in open loop) | The hold lasts until the write-back ends; a fresh 0 frame at the signal's end |
| Disconnect during identification said nothing; the tabs could hide Stop; a re-ARM before START; f32 compare; a bounds check; a moving pre-level; a device change kept the result; a "velocity to velocity" change line | Toast on close; tabs locked while active; ARM epoch checked before the start chain; float compare; bounds check; `steady_start`; result cleared; the line dropped |

A focused re-check of the fixes confirmed the hold balance, that identification always ends, and that no mode write can land while the signal may run. It found:

| Finding | Fix |
|---|---|
| The Firmware card's update, Confirm and Roll back ignored the identification block | The block is in `firmwareActionBlocked()`, so the update and the card's actions wait too (test) |
| Closing after the write-back had finished, while the capture tail still ran, said the mode might not be written back | No toast once the write-back was queued and has ended |
| A dropped Tune change line removed the param from the changed set while a Layout line still named it | The param stays changed while another line names it |

Accepted, documented: K is per unit of the commanded input (a controller duty limit below "to" lowers it). The ARM epoch is checked before CONFIG, not again before START: a DISARM followed by a new ARM within the CONFIG round trips (milliseconds) is not caught; a DISARM alone still ends the run, and the device refuses START while disarmed. A refused mode write (nothing runs) has no vdev test, since the mock accepts every mode value.

Found while building: on desktop vdev the fit took the return to 0 for the step (sample time lags device time), the aliased velocity failed the noise tests, and a mode change just before Run could be read stale from the snapshot (now read from the device; a start waits for Tune writes).

### Results

- **vdev.** `identifyOperations` (mode switched and written back, input back at 0, K ≈ 10 on the mock, Apply, a pre-level, Stop and disarm mid-step, open_loop already set, Save, Rename and the firmware update blocked), `identifyLogic`, `identifyView`; `benchIdentify` dry run on the sample vdev; scenario `tune-identify` at both sizes and themes.
- **Gate.** `python tools/validate.py full` passed (gen, native, host-win, host-linux, fw, fw-nolog, fw-hil-app). A first run had one host-win `motor_smoke` stall assertion fail (unrelated to this step); it did not repeat.
- **Deferred.** `benchIdentify` with a real motor (row in the status file).
