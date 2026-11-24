# DriveLink GUI design record

The Figma design (phases G0–G8, 2026-11-11 to 2026-11-15) is complete and built in H2a–H3b. This file keeps what a later GUI change must respect: the decisions, the ground rules, the state inventory and the screen map. The phases, frame lists, sample device and the open issues (all closed by the build) are in [archive/gui-design-history.md](archive/gui-design-history.md); the Figma node ids, file rules and traps are in [archive/gui-design-status.md](archive/gui-design-status.md). What was built, and how, is in [host/gui/README.md](../host/gui/README.md).

Read with [host-architecture.md](host-architecture.md), section "`drivelink-gui` (Qt/QML)", and [firmware-architecture.md](firmware-architecture.md) for what the device exposes (arming, faults, params, groups, capture, test signals, layout, safe mode, OTA).

**Figma files** (team `drivelink`):
- Design: [DriveLink GUI](https://www.figma.com/design/49NRHfKe8rOoVlloLLCqLh). Pages: Cover, G0 Foundations, G1 Wireframes, G2 Design system, Screens – Local, Screens – Remote, Custom panels, Prototype, Handoff.
- Flows: [DriveLink GUI – Flows](https://www.figma.com/board/bB3a7SnibAjHCvw6wuLPlO) (FigJam). The screen map and the key flows, one section each.

## Decisions

| Topic | Decision |
|---|---|
| Process | Wireframes first: flows and low-fi wireframes for every screen, then the design system, then hi-fi screens, then a clickable prototype. |
| Component library | Custom and small. Figma variables for colour, spacing, radius and type. Components named and shaped like Qt Quick Controls (Button, SpinBox, ComboBox, Switch, TextField, ...) so each maps to one QML control. Dense, desktop, data-heavy. |
| App shell | A left nav rail (Device, Layout, Tune, Monitor) and a top status bar on every screen: connection, arm state, faults, pending-reboot and unsaved flags, lease (remote), and a large DISARM button with a keyboard shortcut. |
| Theme | Dark first. Tokens have dark and light modes from the start; screens are designed in dark and checked in light. |
| Frame size | 2560×1440. Every hi-fi screen is also checked at 1920×1080. |
| Remote mode | Designed in full now (agent connect, lease, client list, ROS-driving states), although it is built in H3. |
| Live params without the lease | Allowed. While another client (ROS) holds the lease, param editors stay editable behind a warning that the robot is being driven and changes apply live. Resolves the host plan's open question. |
| Agent history | The agent keeps recent diag events and arm-state changes, so a GUI that connects later sees them in its Faults view. Captures stay with the client that ran them. Resolves the host plan's open question. |
| Tuning sessions | Saved as files on the PC: runs (capture + the params used + test signal) can be named, overlaid, exported to CSV, and saved and opened as a session file. Resolves the host plan's open question. |
| Custom panels | One reference panel, the `imu.bno055` calibration panel, designed after the generic screens. The generic screens define the slot where a custom panel appears. |
| Command group | The GUI sets up its own command group at connect (every input it can command, at hold values) and sends keepalives, so `ARM` is never refused for "no command group". Accepted in G0. |
| Manual command | Monitor has a Manual command panel to set inputs (`velocity_sp`, `duty_sp`, ...) by hand while armed, through that command group. Accepted in G0. |
| DISARM key | Esc, always, on every screen, even while a text field has focus (the field's edit is cancelled too). G1. |
| Layout view | Instances listed by loop, indented under the controller they are wired to, with a detail pane (params, wiring, signals, actions, custom panel tab). G1. |
| Tune layout | Plot-centred, three columns: controller and gains · plot and run metrics · test signal, capture, runs and session. G1. |
| Faults panel | A right-side drawer that opens from the status bar on every screen and leaves the page visible. G1. |
| DISARM over the agent | Any client may disarm, with or without the lease, so DISARM is enabled in the GUI whenever the device is armed. Decided by the user after G1; recorded in the host plan's agent section. |
| Banners | Page-wide banners (safe mode, pending reboot, ROS driving, first boot) sit full width under the status bar, on every screen they apply to. G1. |
| Type | Inter for the UI, with tabular numerals (`font.features: { "tnum": 1 }`, Qt 6.6+) on every live or editable number; JetBrains Mono for hex, ids and raw frames. Both OFL, bundled in the Qt resources. G2. |
| Colour language | State colours are for state only: Disarmed neutral grey, Armed amber, Fault red, Pending violet, Unsaved yellow dot, Locked grey with a lock, ok/applied green. The accent (focus, selection, primary button) is blue. The DISARM button is always red. G2. |
| Density | Standard: 32 px controls and table rows, 14 px body text, 4 px spacing grid. G2. |
| Qt style | A custom DriveLink style built on the Basic style: each control's `background` and `contentItem` drawn from a `Theme` singleton holding the tokens; dark and light swap the token set. G2. |
| Icons | Lucide (ISC), bundled as SVG; 16 and 20 px, 2 px stroke scaled, one stroke colour. G2. |
| Tokens to QML | Variables are named for code (`color/bg/surface`, `space/4`, `radius/md`). In G8 they are exported to JSON and a script writes the `Theme` singleton with a dark and a light set; Figma is the source of truth. G2. |
| Sliders | A slider beside the numeric field for the controller gains on Tune and the inputs of Monitor's manual command. Generic param rows (Layout) stay field-only. Decided after G2. |
| Gain slider | Log scale over a window of ÷4 to ×4 around the value at the start of the drag, re-centred on release. Min and max can be typed, are clamped to the descriptor range, and are saved in the tuning session file. Sends live while dragging, at most every 50 ms (latest value), and once on release. Ticks on the track mark the saved (NVS) value and the gains of each visible run. |
| Gain steps | Buttons ÷2, −10%, +10%, ×2 beside each gain. Keys: arrows 1%, Shift 10%, Ctrl 0.1%. Steps are percentages because gains span decades. |
| Gain history | Every applied gain set is listed with its time and the run it produced; one click restores a set, and two sets can be compared A/B with their runs overlaid. Kept in the session file. |
| Auto re-run | While armed with a test signal set up, a gain change starts the test signal and capture again once the gains have been still for 0.5 s; a run in progress is cancelled if a gain changes again. The new run is overlaid. |
| Run overlay | The newest run in full colour, the two before it faded, plus pinned runs. Older runs stay in the list and the session file, hidden. |
| Manual command slider | Linear and symmetric around 0, from −limit to +limit. The limit starts at 25% of the descriptor max, can be raised up to it, and is remembered per input in the app settings. Releasing the slider ramps the setpoint back to 0 (dead-man); a per-input Latch toggle keeps the value instead (named Latch, not Hold, because "hold" already means an input left at its hold value: Remove and Hold all). Esc still disarms. |
| G2 accepted | The design system and the tuning controls added after it were accepted by the user on 2026-11-12, before G3 started. |
| Status bar | Three zones: left the DeviceChip (name, id, port); centre the StateChip with the last-disarm reason, the Arm / Ack fault button, the faults badge and the Unsaved and Pending flags; right the red DISARM with Esc. The arm state keeps a fixed place in the middle. Built as the `StatusBar` component. G3. |
| Save and Revert | While `nvs_dirty` is set, the status bar shows an Unsaved badge followed by small Save and Revert buttons: one click from any screen, disabled with a tooltip while armed. G3. |
| Loops and CPU load | The Device screen shows the loop table (rate, run time, overruns) and one ProgressBar per core with its %. A rising overrun count is shown in the fault tone. G3. From G7b the table has one row per running loop and more columns (see "G7b Loops and sources rework"). |
| Size and theme checks | Every hi-fi screen is checked at 1920×1080 and in Light. Copies kept on the page as the record: Connect and Device · Disarmed at 1920×1080, Connect and Device · Fault (drawer open) in Light. Other copies are deleted after the check. G3. |
| Connect layout | A centred 720 px card: tabs Serial / Agent, the port list, baud and Connect, then Recent devices (name, id, port, firmware, last seen). The shell stays visible around it with the nav rail disabled. G3. |
| Rename | A pencil button next to the device name opens a small Dialog with a TextField and the rules. The name applies at once and is saved with the rest of the config on SAVE (it sets Unsaved), like a persist param. Needs the firmware to-do [todo-device-name.md](archive/todo-device-name.md). G3. |
| OTA frames | Three frames: the dialog while writing (steps, progress, retries), waiting to confirm after the reboot, and rolled back with the reason. Refused while armed is a disabled button with a tooltip on the Armed frame. G3. |
| Link lost | A modal over the dimmed, frozen page: the device stopped answering, the retry count, Retry now and Disconnect. The StateChip shows Offline. G3. |
| DISARM availability | While connected, DISARM is always enabled and red, also when the device is already disarmed or in Fault (sending it is harmless). It is disabled only with no link (no device, link lost). Small choice made in G3; ArmButton gained a "Disarm disabled" variant. |
| Modals and DISARM | Dialogs and overlays dim the page with a scrim that starts under the status bar, so the status bar and DISARM stay live above every modal. New variable `color/bg/scrim` (dark in both modes). G3. |
| Reboot notice | An unexpected reset while connected shows a modal: reset reason, instances, safe-mode count, that the reboot disarmed, whether the model was reloaded, and that unsaved (live-only) changes were lost. A toast says the model was reloaded. G3. |
| G3 accepted | The Shell, Connect and Device screens and the G2 additions were accepted by the user on 2026-11-12, before G4 started. |
| Layout frames | G4 covers the instance detail, the wiring picker open, the custom panel slot, pending changes with a refused op, the review list and Discard all, remove with dependents, a failed instance, Layout while armed, and first boot with Add instance. G4. |
| Remove with dependents | **Superseded on 2026-11-13 by "Remove follows the firmware" below.** The G4 decision was: Allowed. A destructive dialog lists the instances wired to it. The remove is staged; the dependents' wiring shows Invalid and the layout is invalid (ARM blocked) until they are rewired or the pending changes are discarded. G4. |
| Wiring picker | Lists every instance of the right type. Those in the same loop can be picked; those in another loop are listed disabled with the reason ("sensor loop; left_drive runs in control"). A `LoopMismatch` refusal from the device still shows on the row. G4. |
| Instance detail | The G1 layout: a header (label, type, loop, state, Rename, Remove), tabs Params plus the custom panel's tab when the type has one (`imu.bno055`: Calibration), and a right column of cards: Wiring, Signals (live), Actions. G4. |
| Loop at add and remove with dependents | **Superseded on 2026-11-13 by "Loop at add" and "Remove follows the firmware" below; the to-do is closed.** The G4 decision was: Add instance shows a Loop combo when the type may run in more than one loop, fixed text otherwise. The firmware plan has no loop argument on `ADD_INSTANCE` and does not say what `REMOVE_INSTANCE` does with dependents, so both go to [todo-layout-ops.md](archive/todo-layout-ops.md) for the firmware side. G4. |
| Pending review | The pending-reboot banner has Review, which opens a list of every staged change (adds, removes, labels, pins, wiring, old → new). There is no per-change undo, because `DISCARD_PENDING` drops everything: Discard all asks to confirm and lists what is dropped. Markers stay on the tree and the rows. G4. |
| Add flow | The Add instance dialog asks for the type, the label and (when there is a choice) the loop. Add stages it and selects the new instance; its pins and wiring are then edited in the detail pane, with unset wiring marked until it is filled. G4. |
| G4 accepted | The Layout screens and the G2 additions (Banner buttons 2 and 3, MenuItem detail) were accepted by the user on 2026-11-12, before G5 started. |
| G5 accepted | The user accepted Tune and Monitor, the size/theme checks and the Slider Drag start variant on 2026-11-13. Auto re-run remains on by default and activates only after the first manual Run in the connection. The documented 1920×1080 issues remain open for handoff. |
| Remote lease placement | The lease chip sits beside the device identity, opens lease details and navigates to the Clients and lease card on Device; the arm state stays centred. G6, user decision 2026-11-13. |
| Remote live edits | Live gains remain editable while another client holds the lease. Confirm the first edit, offering Don't ask again for this connection, and keep a persistent warning banner. Run and manual commands require the lease. G6, user decision 2026-11-13. |
| Clients and lease | A card on Device holds the client list and Take / Release lease controls; the status-bar lease chip navigates there. G6, user decision 2026-11-13. |
| G6 acceptance | Accepted 2026-11-13: all 16 Remote screens, 1080p Light checks and the G2 LeaseChip, StatusBar Lease, Dialog Confirmation and PlotFrame Remote live extensions. User requested a PR into firmware_overhaul, then G7 in this session. |
| G7 layout | Dashboard with four live calibration levels, guidance and file/actions cards. User decision 2026-11-13. |
| G7 calibration files | Import locally, review device/file details, then explicitly Apply to IMU. Read from IMU and Export are separate actions. Import alone sends nothing. User decision 2026-11-13. |
| G7 orientation | Numeric orientation values plus a simple orientation graphic. User decision 2026-11-13. |
| Tune and Monitor frames | G5 covers Tune: run results (armed, a gain being dragged, runs overlaid, metrics, gain history), disarmed, run in progress and a run stopped by a disarm, A/B compare, plant identification; Monitor: live plots with the manual command, paused with cursors, link budget full. G5. |
| Gain of 0 | The slider and the % steps are disabled with a hint to type a value first; once the gain is non-zero the log window works. G5. |
| Negative gains | When a gain's descriptor allows min < 0, the log slider and the % steps act on the magnitude and keep the sign; crossing zero is only by typing. The cascade_pid gains are >= 0, so no frame shows it. G5. |
| Auto re-run default | The chip is on by default, but auto re-runs start only after the user has pressed Run once in this connection, so a gain change never moves a motor before a run the user started. G5. |
| Run while disarmed | Run is disabled with an "Arm first" tooltip pointing to ARM in the status bar. Arming stays one click in one place; gains, the test signal and the capture can still be set up. Replaces the wireframe's "Arm and run". G5. |
| Run colours | Each run takes the next series colour when it is made and keeps it, so the plot, the runs table, the metrics and A/B match. The newest run in full, the two before it faded. G5. |
| Monitor plots | Stacked full width with one shared time axis, so pause and the cursors line up across plots; one plot per unit, Add plot adds one. G5. |
| Link budget | The signal browser shows what a signal would add to the link budget. Over 100 % or over 8 subscription groups, the add is refused with the reason and a one-click lower plot rate. Nothing changes by itself. G5. |
| Save on Tune | Tune has no Save button of its own: Save and Revert stay in the status bar (G3). The gains card has "Revert gains to saved", which sets the saved gains again live (a GUI action, not `REVERT`). Small choice made in G5. |
| Loops are edited on Layout | A loop is an instance of the built-in `loop` type (firmware F3d). Selecting a loop header in the Layout tree opens it in the detail pane like any instance: its params, its live statistics and the instances that run in it. The Device loop table stays read-only, and each row links to the loop on Layout. G7b, user decision 2026-11-13. |
| Remove follows the firmware | The device refuses to remove an instance that another one is wired to, and a loop that still holds an instance (`Referenced`). So Remove is disabled in those cases, and a tooltip names what must be rewired, moved or removed first. Nothing is staged and the layout does not go invalid. This replaces the G4 "Remove with dependents" dialog. G7b, user decision 2026-11-13. |
| Loop at add | `ADD_INSTANCE` takes a loop id. The Add instance dialog lists the loops the type may run in, by name: a type is `realtime: required`, `forbidden` or `any`, and a loop is real-time or not. The device's default (the lowest-numbered compatible loop) is preselected. With no compatible loop the dialog says so and offers Add loop. G7b. |
| Sources | A source is a reboot-required param that names a signal or input of another instance, in any loop. It gets its own row: a target picker, the sampled value, its age, and the states valid, stale, missing and not connected. Originally provisional (2026-11-13); checked against the built F4c/core v8 contract on 2026-11-14. |
| Sample device has three loops | The sample device gains a `fusion` loop (200 Hz, not real-time) with one fusion instance, so the screens show more than the factory pair. Every frame that shows the instance tree or the loop table is updated. G7b, user decision 2026-11-13. |
| Limits and source state | The device reports its limits as read-only system params (firmware F4b), and a source's value and age are ordinary signals named `<source>.value` and `<source>.age_us` (firmware F4c). The GUI shows counts against the limits, disables Add at the limit, and reads a source's state like any live signal. Settled with the firmware plan on 2026-11-13. |
| G7 review during G7b | Proceed with G7b and leave G7 Custom panel in review. G7 acceptance and reconciliation with the built F6 calibration support remain open. User decision 2026-11-14. |
| G7b Add control | One Add button on the Instances card opens an Instance / Loop menu. User decision 2026-11-14. |
| G7b loop headers | Selectable loop headers show name, rate, a neutral Real-time badge when applicable, and budget % (run time divided by the period). CPU load remains per core. User decision 2026-11-14. |
| G7b obsolete invalid-layout frame | Delete "After remove, wiring invalid" (`89:5729`), without replacing it with a boot-invalid screen. Refused removals leave the layout unchanged. User decision 2026-11-14. |
| G7b cross-loop wiring example | Keep the wiring picker's disabled-with-reason example by adding a second real-time loop to that scenario. `sim_motor` runs there, so its type is compatible with its loop, while wiring it to `left_drive` in another loop is refused. User decision 2026-11-14. |
| G7b source sample | Use the shipped `fusion.mock` type and its actual `feedback` and `imu` source params, generated value/age signals and `combined` signal. Replace the illustrative `fusion.odometry` and its three invented source items. User decision 2026-11-14. |
| G7b scope approved | Parts A–G, including G2 extensions, 17 new Layout frames, shared-screen updates, two G4 deletions, Cover/flow records and size/theme checks, approved by the user on 2026-11-14 before construction. G7 remains in review. |
| G7b accepted | The user accepted G7b and its G2 extensions on 2026-11-14. Parts A–G are done. G7 remains in review; inherited G5 responsive issues remain open for G8/H2. |
| G7 accepted | The user also accepted G7 Custom panel on 2026-11-14, including CalibrationLevel, OrientationPreview, the 11 screens and retained checks. G8 discussion is next; calibration alignment and host contracts remain in the handoff notes. |
| Device settings card (issue 105 step 4) | The editable counterpart of the read-only Limits card, which stays and shows running values. Title "Device settings", on the Device page: two columns wide below Loops, with Firmware and Limits stacked beside it (wide); full width on two columns (narrow). Rows come from the writable system params: ungrouped first, then a heading per name prefix. No Save or Discard in the card; the status bar and the pending banner do that. User decisions 2026-11-24. |
| Device settings in safe mode | Neutral, not pending: an info note in the card, the field shows the saved value, and the row says "Safe mode runs 4 · 6 after a normal boot". While the device reports no pending reboot the pending list leaves these rows out; once something is staged it lists them all, because the device does not tell saved from staged per row. User decision 2026-11-24. |
| Discard after Save | DISCARD_PENDING does not undo a SAVE: the next boot still loads the saved values. A note in the card and one in the Discard dialog say so, and point to Save and reboot after discarding to keep the old values. User decision 2026-11-24. |
| Device settings accepted | The 13 frames (10 local, 1 narrow, 1 compact Light, 1 remote observer) were accepted by the user on 2026-11-24, before any GUI code. |
| Figma account | The files live in the `drivelink` team (Education plan: unlimited pages). The MCP seat is still a View seat, so reads (screenshots of the canvas) are kept for final checks; diagrams are checked from the thumbnails `generate_diagram` returns. |

## Ground rules for every design

- **Generic.** No screen is built for a particular module. Everything shown comes from the descriptors: types, instances and labels, items with kind, type, unit, range, enum names and flags, diag codes with names and severity. Designs use a sample device (below) but must work for any type.
- **Safety first.** Arm state and DISARM are visible and one click (or one key) away on every screen, in every state. Anything that is refused while armed looks disabled while armed and says why.
- **Every refusal explains itself.** `ARM` refused, a param clamped, a batch rejected (`CrossLoopBatch`, `StagingBusy`, `LockedWhileArmed`), a layout op rejected (`GpioConflict`, `LabelInUse`, `LoopMismatch`): the UI shows the reason next to the thing that was refused.
- **Pending vs live.** `reboot_required` params and layout ops go to a pending copy; live params apply at once. The two must never look alike.
- **Unsaved vs saved.** `nvs_dirty` means live changes are not in NVS yet. SAVE and REVERT are always reachable while it is set.

## State inventory

The states the designs must cover, completed in G0. G1 gives each one a place on screen.

| Area | States |
|---|---|
| Connection | no ports found, port busy or open failed, connecting, loading the model (`GET_INFO` → version check → types → items → codes → instances → params, with progress), connected, link lost → reconnecting (auto retry), protocol mismatch (refused, host and device versions shown), descriptor hash changed (model reloaded), device rebooted (Boot event: reset reason, instances ok and failed, safe mode) |
| Arm | Disarmed (with the last disarm reason: explicit, watchdog, lease lost, reboot), arming (request in flight), Armed, `ARM` refused with reasons (fault latched, `blocks_arm` code active, layout invalid, pending reboot, instance failed, safe mode, no command group), Fault (latched, with the code), `ACK_FAULT` refused (a `disarms` code is still active) |
| Device flags | `pending_reboot`, `nvs_dirty`, `layout_invalid`, `safe_mode`, `testgen_running`, capture state, config not loaded from NVS (defaults), no layout yet (zero instances, first boot), CPU load per core, loop overruns rising |
| Faults (diag) | active, cleared, history, severity, kind (record, latched, counter with its count), `blocks_arm` and `disarms` badges, per instance or device-wide, name and description from the device, from the agent's history (remote) |
| Param row | normal, edited (not sent), sending, applied, applied but clamped (applied value shown), rejected (non-finite, invalid enum), busy (`StagingBusy`, retry), cross-loop batch refused, pending reboot (staged vs current), locked while armed, read-only, changed since the last save (tracked by the GUI). Types: f32 and integers with unit and range, enum, bool, `Ref` (picker), source (signal picker, from G7b) |
| Instance | running, failed to start (`InitFailed`), invalid at boot (failed layout validation), left out (`DependencyMissing`), offline (safe mode), pending add, pending remove, label change pending, has an active fault |
| Loop (G7b) | running (real-time or not), failed to start (its instances are `DependencyMissing`), offline (safe mode: no loop runs), pending add, pending remove, pending change (`realtime`, `core`, `priority`, `stack_bytes` need a reboot), rate changed live (unsaved), rate locked while armed, overruns rising, over budget (run time above the period), no loop at all (a saved empty layout) |
| Source (G7b) | not connected, waiting for first sample, valid (value and age), stale (age above the displayed host threshold or a consumer limit), missing (the producer is not running), telemetry unavailable, pending (a new target after the reboot), refused (`BadRef`, `WiringCycle`, `Capacity`). `fresh` is internal to the module and is not a separate wire signal. |
| Layout op | accepted (pending), rejected (`GpioConflict`, `LabelInUse`, `LoopMismatch`, wrong `Ref` type, empty label, `Referenced` on a remove, `Capacity` when the loop or instance limit is reached), discard pending |
| Action | idle, running, done with outputs, failed (result, detail), refused while armed (`paused` actions on an instance in a real-time loop) |
| Capture | not configured, Idle, Armed (waiting for the trigger), Triggered (recording), Done, reading (progress), read failed, config over capacity |
| Test signal | stopped, running (progress), stopped by disarm, refused (not armed, target not a testgen input) |
| Manual command | no command group, inputs held, sending (keepalive when idle), NaN shown as hold. Built in H2d-2: disabled with the reason (not armed by this window, another owner, setup failed), held at 0 while a param write is in flight, refused inputs listed with the reason |
| Live plots | subscription groups used (n of 8), link budget %, packet loss (counter gap), paused, signal from a stalled loop |
| Tune session | new and unsaved, saved to a file; per run: visible or hidden, colour, params snapshot, differs from the current params; export to CSV |
| OTA | choose image, refused while armed, writing (progress, retries), verifying (SHA-256), rebooting, reconnecting, waiting to confirm, confirmed, rolled back, aborted or failed |
| Safe mode | banner on every screen; allowed: edit layout and params, factory reset, OTA, exit safe mode; ARM unavailable |
| Agent (remote) | agent unreachable, agent version mismatch, connected but no device on the agent, client list |
| Lease (remote) | nobody holds it, held by me, held by ROS (driving), held by another GUI, requesting, granted, revoked (and by whom), lost when I disconnect, param edit while another client drives (warning) |

## Screen map

From G0 (FigJam, section "G0 · Screen map"):
- **Connect:** serial port or agent address → loading the device model → the shell. A protocol mismatch stops here.
- **Shell, on every screen:** status bar (connection, arm state, ARM / DISARM / acknowledge, faults, save / revert / pending reboot, lease chip) and nav rail. A faults drawer opens from the status bar. Overlays for link lost, device rebooted and safe mode.
- **Device:** info, loops and CPU load; faults and history; reboot and factory reset; firmware update; clients and lease (remote).
- **Layout:** instances grouped by loop; add instance; instance detail with the param table, wiring pickers, actions and the custom panel slot. From G7b: add and remove loops, loop detail, and source rows.
- **Tune:** controller and gains; test signal and capture; runs and overlay; session file; plant identification.
- **Monitor:** live plots, signal browser, manual command (proposed).
- **App settings:** from the bottom of the nav rail.

## Key flows

In the FigJam file, one section each: 1 connect and load, 2 arm state machine, 3 fault and acknowledge, 4 edit a live param, 5 first-boot layout with save and reboot, 6 tune run, 7 plant identification, 8 safe mode recovery, 9 firmware update, 10 remote lease handoff (sequence), 11 link lost and device rebooted.
