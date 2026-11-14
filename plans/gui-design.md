# Plan: DriveLink GUI design (Figma)

Status: draft, 2026-11-11. Design only: no QML or C++ is written until the firmware overhaul and `libdrivelink` (H1) are done. The GUI itself is built in H2 (local mode) and H3 (remote mode), from these designs.

The working state (current phase, next step, handoff note, Figma node ids) is in [gui-design-status.md](gui-design-status.md). Continue the work with `/gui-design-step`.

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
| Rename | A pencil button next to the device name opens a small Dialog with a TextField and the rules. The name applies at once and is saved with the rest of the config on SAVE (it sets Unsaved), like a persist param. Needs the firmware to-do [todo-device-name.md](todo-device-name.md). G3. |
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
| Sources (provisional) | A source is a param that names a signal of another instance, in any loop (firmware F4c, planned but not built). It gets its own row: a signal picker, the sampled value, its age, and the states valid, stale, missing and not connected. Designed now and marked provisional; checked again when F4c lands. G7b, user decision 2026-11-13. |
| Sample device has three loops | The sample device gains a `fusion` loop (200 Hz, not real-time) with one fusion instance, so the screens show more than the factory pair. Every frame that shows the instance tree or the loop table is updated. G7b, user decision 2026-11-13. |
| Limits and source state | The device reports its limits as read-only system params (firmware F4b), and a source's value and age are ordinary signals named `<source>.value` and `<source>.age_us` (firmware F4c). The GUI shows counts against the limits, disables Add at the limit, and reads a source's state like any live signal. Settled with the firmware plan on 2026-11-13. |
| Figma account | The files live in the `drivelink` team (Education plan: unlimited pages). The MCP seat is still a View seat, so reads (screenshots of the canvas) are kept for final checks; diagrams are checked from the thumbnails `generate_diagram` returns. |

## Ground rules for every design

- **Generic.** No screen is built for a particular module. Everything shown comes from the descriptors: types, instances and labels, items with kind, type, unit, range, enum names and flags, diag codes with names and severity. Designs use a sample device (below) but must work for any type.
- **Safety first.** Arm state and DISARM are visible and one click (or one key) away on every screen, in every state. Anything that is refused while armed looks disabled while armed and says why.
- **Every refusal explains itself.** `ARM` refused, a param clamped, a batch rejected (`CrossLoopBatch`, `StagingBusy`, `LockedWhileArmed`), a layout op rejected (`GpioConflict`, `LabelInUse`, `LoopMismatch`): the UI shows the reason next to the thing that was refused.
- **Pending vs live.** `reboot_required` params and layout ops go to a pending copy; live params apply at once. The two must never look alike.
- **Unsaved vs saved.** `nvs_dirty` means live changes are not in NVS yet. SAVE and REVERT are always reachable while it is set.

## Sample device for the designs

A two-wheel bench rig, used in every wireframe and screen so the data is consistent:
- loops (each an instance of the built-in `loop` type; the first two are the factory layout, the third is added from G7b on):

  | Loop | Id | Rate | Real-time | Core | Priority | Stack |
  |---|---|---|---|---|---|---|
  | `control` | 0 | 1 kHz | yes | 1 | 20 | 4096 |
  | `sensor` | 1 | 100 Hz | no | 0 | 10 | 4096 |
  | `fusion` | 2 | 200 Hz | no | 0 | 12 | 4096 |

- in `control`: `controller.cascade_pid` `left_drive` and `right_drive`, each wired to an `actuator.pwm_dir` (`left_pwm`, `right_pwm`) and a `feedback.quadrature` (`left_enc`, `right_enc`)
- in `sensor`: `imu.bno055` `imu`
- in `fusion` (from G7b): `fusion.odometry` `odom`, with three sources: `left` → `left_enc.count`, `right` → `right_enc.count`, `heading` → `imu.yaw`. The type and its item names are illustrative; the firmware plan only says "a fusion type".
- `actuator.mock` and `feedback.mock` `sim_motor` (for safe testing)
- example faults: `feedback.SignalLost` on `right_enc` (active, disarms), `core.WatchdogTimeout` (history)

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
| Source (G7b, provisional) | not connected, valid (value and age), fresh or held, stale (age above the consumer's limit, or the producer loop paused or stalled), missing (the producer is not running), pending (a new target after the reboot), refused (unknown signal, wrong value type: `BadRef`) |
| Layout op | accepted (pending), rejected (`GpioConflict`, `LabelInUse`, `LoopMismatch`, wrong `Ref` type, empty label, `Referenced` on a remove, `Capacity` when the loop or instance limit is reached), discard pending |
| Action | idle, running, done with outputs, failed (result, detail), refused while armed (`paused` actions on an instance in a real-time loop) |
| Capture | not configured, Idle, Armed (waiting for the trigger), Triggered (recording), Done, reading (progress), read failed, config over capacity |
| Test signal | stopped, running (progress), stopped by disarm, refused (not armed, target not a testgen input) |
| Manual command | no command group, inputs held, sending (keepalive when idle), NaN shown as hold |
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

## Phases

Each phase ends with a review by the user before the next one starts. Each phase updates this file: its state, the decisions made, and links to its Figma pages.

| Phase | Content | Output | State |
|---|---|---|---|
| G0 Foundations | Figma file and page structure. The screen map (information architecture). The state inventory completed. Key flows as FigJam diagrams: first connect and descriptor load, first-boot layout setup, save and reboot, arm with refusal, fault and acknowledge, tune run, safe mode recovery, OTA, remote lease handoff. | FigJam flows, screen map, this file updated | done |
| G1 Wireframes | Low-fi, greyscale wireframes at 2560×1440 for the shell and every screen: Connect (serial and agent), Device (info, arm, faults, save/revert, reboot, OTA, safe mode), Layout, Tune, Monitor, and the overlays and banners from the state inventory. Remote states included. | Wireframes page | done |
| G2 Design system | Variables (colour in dark and light, spacing, radius, type scale with tabular numerals) and semantic state colours (armed, disarmed, fault, pending, unsaved, locked). Components mapped to Qt Quick Controls: buttons, numeric field with unit, range and clamp feedback, enum combo, switch, text field, `Ref` picker, param row with its markers, status bar chips, DISARM button, nav rail, instance tree item, table, banner, dialog, toast, plot frame (axes, legend, cursors), signal chip. A QML mapping table. | Design system page | done |
| G3 Shell, Connect and Device | Hi-fi shell (status bar, nav rail), the Connect screen (serial; the agent tab is finished in G6), and the Device screen with faults, arming, save/revert, reboot, OTA and safe mode. Checked at 1920×1080 and in light. | Screens page | done |
| G4 Layout | Hi-fi Layout screen: instance tree by loop, add and remove instances, labels, wiring with `Ref` pickers, GPIO and validation errors, the pending-reboot banner and discard. The param table for an instance, and the custom panel slot. | Screens page | done |
| G5 Tune and Monitor | Hi-fi Tune screen: pick a controller, live gains, test signal and capture set-up, run, overlay runs, the session file (name, save, open, export CSV), plant identification and suggested gains, save or revert. Hi-fi Monitor screen: free-form live plots of any signals, subscription set-up, pause and cursors. | Screens page | done |
| G6 Remote mode | Agent connect, the lease chip and its states, take and release the lease, lease revoked, the client list, the warning on param edits while another client drives, diag history on connect. | Screens – Remote | done |
| G7 Custom panel | The `imu.bno055` calibration panel in its slot, as the reference for `gui/modules/<type>/`. | Custom panels | in review |
| G7b Loops and sources rework | Bring G2 to G7 in line with the firmware's configurable loops (F3d) and planned sources (F4c): loop items in the Layout tree, loop detail, add and remove loop, the reworked Add instance dialog and Remove rule, the Device loop table, source rows, and the three-loop sample device on every frame that shows loops. See "G7b Loops and sources rework". | Design system, Screens – Local, Screens – Remote, Custom panels | planned |
| G8 Prototype and handoff | A clickable prototype of the key flows from G0. A handoff page: screen and component specs, the QML mapping, and the open issues for H2 and H3. | Prototype and handoff pages | planned |

## G1 wireframes

On the "G1 Wireframes" page: a kit of greyscale components on the left (button, field, dropdown, chip, banner, param row with 10 states, tree row, nav rail, status bar with Offline / Disarmed / Arming / Armed / Fault, plot), and 21 frames at 2560×1440 to the right, with an index card above them. Yellow notes are annotations.

| # | Frame | Covers |
|---|---|---|
| 1–3 | Connect: serial port, loading the model, protocol mismatch | connection states, first-connect progress |
| 4–8 | Device: disarmed, ARM refused, fault with the faults drawer, firmware update, safe mode | arming, ARM checks, faults, OTA, safe mode |
| 9–11 | Layout: instance detail, pending changes, first boot with Add instance | param row states, wiring, pending vs live, layout refusals |
| 12–13 | Tune: run results, plant identification | runs, overlay, metrics, session file, suggested gains; gain sliders, steps, auto re-run and gain history (updated after G2) |
| 14 | Monitor: live plots and manual command | signal browser, rates, link budget; command sliders with limit, Latch and dead-man release (updated after G2) |
| 15–16 | Overlays: link lost, device rebooted and model reloaded | flow 11 |
| 17 | Settings | theme, safety, connection, sessions |
| 18–21 | Remote: connect to an agent, Tune while ROS holds the lease, take the lease, clients and lease | flow 10, the live-param warning, agent history |

## G2 design system

On the "G2 Design system" page, as Figma sections: About G2, Foundations, Preview, QML mapping and Icons on the left; the component sections to the right.

**Variables** (local, 4 collections):

| Collection | Modes | Content |
|---|---|---|
| Primitives | Value | 61 colours (grey ramp, blue, amber, red, green, violet, yellow, 8 chart colours for dark and light). Hidden from pickers. |
| Color | Dark, Light | 50 semantic colours aliased to primitives: `bg/*`, `border/*`, `text/*`, `accent/*`, `state/*` (disarmed, armed, fault, ok, pending, unsaved, locked, each with a subtle fill), `disarm/*`, `plot/*`, `series/1..8`. |
| Size | Value | `space/0..64` (4 px grid), `radius/none, sm 3, md 4, lg 8, full`, `size/control 32, control-sm 24, icon-sm 16, icon-md 20, statusbar 56, navrail 72, disarm 40`, `border/width 1, focus 2`. |
| Type | Value | `type/family/ui` Inter, `type/family/mono` JetBrains Mono, sizes 11–24, line heights 16–32. |

Every variable has scopes set, and its description gives its QML name (`Theme.bgSurface`, `Theme.space8`). Figma has no QML code-syntax platform, so the names live in the descriptions; the G8 export script derives them the same way.

**Styles:** 11 text styles bound to the Type variables (Display/Value, Title/Page, Title/Section, Body/Default, Body/Strong, Body/Numeric, Label/Default, Label/Caps, Caption, Mono/Default, Mono/Small). 2 effect styles (Elevation/Popup, Elevation/Dialog).

**Icons:** 67 Lucide 0.460 icons as 16 px components (`Icon/<name>`), stroke bound to `color/text/secondary`, swapped through instance-swap properties.

**Components** (all colours, spacing, radii and heights bound to variables):

| Section | Component sets (variants) |
|---|---|
| Buttons | Button (4 styles × 5 states), IconButton (2 sizes × 4 states), ArmButton (Arm, hover, disabled, Arming, Disarm, hover, pressed, Ack fault) |
| Inputs | TextField (5), SpinBox (9: default, hover, editing, edited, clamped, rejected, disabled, read-only, pending), ComboBox (4), MenuItem (4), Menu, Switch (2 × 3), CheckBox (3 × 2), RefPicker (5) |
| Status | StateChip (Offline, Connecting, Disarmed, Arming, Armed, Fault, Safe mode), Badge (8 tones × 2 sizes), DeviceChip (Serial, Agent; name and id) |
| Param row | ParamRow (12 states from the inventory), plus the editor swapped by item type (enum, bool, Ref) |
| Navigation | NavItem (3), NavRail, TabButton (3), TreeItem (loop header + 7 instance states × 2 depths) |
| Table | TableHeaderCell (2), TableCell (3 types × 3 states), a fault-history example |
| Feedback | Banner (Info, Pending, Armed, Fault), Dialog (Default, Destructive), Toast (3), Tooltip, ProgressBar (2), CheckItem (4, added in G3) |
| Shell (added in G3) | StatusBar (8 states; Unsaved, Pending reboot and Faults on or off) |
| Plot | SignalChip (On, Off), PlotFrame (Live; Paused with A–B cursors and a readout), with a real velocity step response |
| Tuning controls | Slider (Log, Symmetric × default, hover, dragging, disabled; saved and run markers, value bubble), GainControl (default, dragging live), CommandControl (released, dragging, latched) |

**Preview:** one sample panel built only from instances (device chip, Armed state, DISARM, ROS-driving banner, tree, tabs, six param rows, a paused plot), shown in Dark and again in Light by switching the Color mode on the frame.

**QML mapping:** a table on the page maps each component to its `DL*` QML type and the Qt Quick Control it is built on. Points that matter for H2:
- The Figma `SpinBox` becomes `DLNumberField` (a `TextField` with a validator), because QML `SpinBox` is integer-only.
- `DLArmButton` owns the application-wide Esc shortcut that always sends DISARM.
- `DLParamRow` picks its editor with a `Loader` by item type.
- `DLPlot`: QtGraphs if it keeps up with the subscription rates, otherwise a custom scene-graph item. Decided in H2.

## G3 Shell, Connect and Device

On the page "Screens – Local", 15 hi-fi frames at 2560×1440 in Dark, in rows, with a header and row labels on the left. Built only from G2 instances; layout containers (cards, columns) are frames bound to the variables.

**Added to G2 for G3:**
- `StatusBar` (new section "Shell"): 8 variants (No device, Loading, Disarmed, Arming, Armed, Fault, Safe mode, Link lost); booleans Unsaved, Pending reboot, Faults. Left and right zones share the free width so the centre group stays centred. QML `DLStatusBar`.
- `CheckItem` (Feedback): Pass, Fail, Active, Pending, with Label and Detail. Used for ARM checks, model loading and OTA steps. QML `DLCheckItem`.
- ArmButton: new variant "Disarm disabled" (no link only).
- ProgressBar: the Track is now auto-layout; an instance sets its value with the Track's right padding (`width × (1 − value)`).
- Variables: `yellow/700` and the Light value of `color/state/unsaved` moved to it (the Unsaved badge was low contrast in Light); `color/bg/scrim` with primitives `scrim/dark` and `scrim/light`.

| Row | Frames | Covers |
|---|---|---|
| Connect | Serial port, Loading the device model, Protocol mismatch | port list (known device, busy port), baud, recent devices by name and id (a device without a name shows its id), load steps with progress, the refused connect with both versions |
| Device · states | Disarmed with unsaved changes, Armed, ARM refused, Fault with the faults drawer | device info with Rename, loops table and core bars, ARM checks, faults table, configuration (Save, Revert, Reboot, Save and reboot, Factory reset), firmware card; refused-while-armed buttons with tooltips; the ARM refused popover under the status-bar Arm button with a fix link per reason; the drawer with the active fault, its flags, the device-served description, Acknowledge refused while active, and today's history |
| Device · safe mode and dialogs | Safe mode, Rename, Factory reset | the full-width safe-mode banner with Exit safe mode, ARM unavailable; the rename dialog with its rules; the destructive factory-reset dialog |
| Device · firmware update | Writing, Waiting to confirm, Rolled back | steps, progress and retries; confirm after reconnect; the rollback reason |
| Overlays | Link lost (over Armed), Device rebooted and model reloaded | flow 11 |
| Checks | Connect and Device · Disarmed at 1920×1080; Connect and Device · Fault (drawer) in Light | the kept record; every other screen was checked at 1920×1080 in Light and the copies deleted |

## G4 Layout

On the page "Screens – Local", below the G3 rows: a "G4 · Layout" header, 11 hi-fi frames at 2560×1440 in Dark in three rows, and a checks row. Built by cloning the G3 Device frame, so the StatusBar and NavRail stay instances (NavRail: Layout selected).

**The Layout screen:** an Instances card on the left (Add instance, then TreeItems by loop, with CPU load on each loop header; actuator and feedback indented under their controller) and the instance detail on the right: the header (label, type · loop · state, Rename, Remove instance), the tabs (Params, plus the custom panel tab), the params table (filter chips All / Changed / Pending, a filter field, rows grouped by name prefix) and a right column with the Wiring, Signals (live) and Actions cards. Wiring rows are a name and a RefPicker, not a ParamRow, because the picker is wider than the ParamRow editor.

**Added to G2 for G4:**
- Banner: two more optional buttons on every variant, booleans `Button 2` and `Button 3` (off by default), for the pending banner's Review / Save and reboot / Discard all.
- MenuItem: an optional second line (`Show detail`, Detail in Caption), used by the wiring picker for the type, the loop and why an entry is disabled. A one-line item is still 32 px; with the detail it is 44 px.
- The QML mapping rows for Banner and Menu / MenuItem say so. No variable changes.

| Row | Frames | Covers |
|---|---|---|
| Instance detail | Instance detail (left_drive), Wiring picker open, Custom panel slot (imu, Calibration tab), Armed with the layout locked | param rows Edited, Unsaved, Clamped; the picker with `sim_motor` (pending add in the sensor loop) listed disabled with the reason; the dashed slot for `gui/modules/imu.bno055/`; while armed: Add, Rename, Remove and the wiring disabled with a tooltip, a live gain still applied, a `locked_while_armed` param Locked |
| Pending changes | Pending changes with a refused op, Review pending changes, Discard all confirm | `aux_pwm` pending add; `pwm_gpio` 18 refused with `GpioConflict` naming right_enc (pin_a); pins Pending reboot; right_drive "wiring pending"; the pending banner with three actions; the Review popover listing each staged change (old → new); the destructive confirm |
| Remove, failures, first boot | Remove with dependents, After remove (wiring invalid), Failed instance, First boot with Add instance | the dialog naming left_drive; left_pwm Pending remove, the Fault banner "Layout invalid" with Fix wiring above the pending banner, the RefPicker Invalid with its reason, ARM disabled "ARM blocked: layout invalid"; right_enc InitFailed and right_drive DependencyMissing in the tree, the failure card with Open faults, ARM disabled "right_enc failed to start"; the first-boot banner, the empty tree and the Add instance dialog (type list with their loops, label, Loop combo for actuator.mock) |
| Checks | Instance detail at 1920×1080, Pending changes in Light | the kept record; every other frame was checked at 1920×1080 and in Light and the copies deleted |

## G5 Tune and Monitor

On the page "Screens – Local", below the G4 rows: a "G5 · Tune and Monitor" header, 9 hi-fi frames at 2560×1440 in Dark in three rows, and a checks row. Built by cloning the G4 Instance detail frame (NavRail: Tune or Monitor selected).

**The Tune screen:** three columns. Left: "Controller and gains" (controller ComboBox, mode, the `vel.*` GainControls in a 2×2 grid, Revert gains to saved) and "Gain history" under it. Centre: the toolbar (Run F5, Stop, the run status, the Auto re-run chip, the captured signals), the plot with the runs overlaid, and the run metrics. Right: Test signal, Capture, Runs and Session cards. GainControl and CommandControl are fixed at 344 px, so the gains sit two per row and Monitor's Manual command column is 392 px.

**The Monitor screen:** the signal browser on the left (signals by instance with their live values, plot rates per loop, the link budget, groups n of 8, lost frames), stacked plots in the centre sharing one time axis, and the Manual command card on the right.

**Added to G2 for G5:** Slider `Scale=Log, State=Drag start` (the handle and value bubble at the window centre, as at the moment of the press), because the position of a child inside an instance can't be overridden. The QML mapping row for Slider says so. No variable changes.

| Row | Frames | Covers |
|---|---|---|
| Tune · runs | Run results, Run in progress, Run stopped by disarm, Disarmed (new session) | vel.kp being dragged (bubble, saved and run ticks, re-run after release), vel.kd at 0 with the slider and steps disabled, runs 1–3 in fixed colours (newest full, two faded), metrics, gain history, the unsaved session; run 4 recording with progress and Stop, "changing a gain cancels run 4"; run 4 cut by a disarm with the marker, the capture kept recording, "incomplete run", a toast, Run disabled with "Arm first"; the empty session with "No runs yet" and Auto re-run "starts after your first Run" |
| Tune · compare and identify | A/B compare, Plant identification | the A and B sets marked in the gain history, only their runs plotted, gain differences and metrics side by side, Restore A / Restore B / Exit A/B; the three steps (checks, the open-loop duty step, model and suggested gains), measured vs fitted plot, K / T / L and fit error, Apply suggested gains |
| Monitor | Live plots and manual command, Paused with cursors, Link budget full | the signal browser, rates, link 12 %, groups 2 of 8; one input dragging and one Latched; Resume, the A–B cursors through every plot and the readout table; 16 signals at 1 kHz (97 %), the refused add "would take the link to 101 %" with "Plot control at 500 Hz (53 %)" |
| Checks | Tune · Run results at 1920×1080, Monitor · Live plots in Light | the kept record; the other frames were checked in Light, and frames 3, 5, 7 and 8 at 1920×1080, with the copies deleted |

## G6 Remote mode

On "Screens – Remote" (`2:6`): 16 hi-fi frames at 2560×1440 in Dark, arranged in four rows, plus two retained 1920×1080 Light checks. Built from the G2 shell, controls, tables and plots; the G5 controller/gain and Monitor layouts are reused.

**Added to G2:**
- `LeaseChip` (`133:984`), six variants: Available, Mine, ROS, Other, Requesting, Revoked. Uses existing variables and Lucide icons. QML `DLLeaseChip` / `AbstractButton`; the mapping row is `133:1036` and the instance preview is `133:1045`. Ownership colours remain separate from arm state.
- `StatusBar` has an optional `Lease` boolean, off by default. Its chip sits beside the device identity; activating it navigates to Device > Clients and lease. Existing local screens keep their original shell.
- `Dialog` has an optional `Confirmation` checkbox, off by default, used for takeover consent and the connection-scoped live-edit warning. The existing Dialog mapping row documents it.
- `PlotFrame` has a responsive `Remote live` variant (`140:978`), with scaling plot geometry and a filling header/plot area. The PlotFrame mapping row documents it.

| Row | Frames | Covers |
|---|---|---|
| Agent connect (y 0) | Agent `137:978`, Unreachable `137:1115`, Version mismatch `137:1255`, No device `137:1406` | Agent tab, endpoint, recent agent with device name/id; connection refusal and retry; incompatible agent versions; agent reachable but serial device unavailable, with ARM/DISARM unavailable |
| ROS and consent (y 1640) | Device · ROS lease `138:10168`, Tune · ROS holds lease `140:1715`, Confirm live edit `140:2397`, Take from ROS `138:11284` | Device client card, persistent driving warning, live gains and telemetry; Run/manual commands unavailable; Apply live with Don't ask again for this connection; explicit safe-to-stop checkbox before takeover, primary action disabled until checked |
| Lease transitions (y 3280) | Requesting `138:11399`, Lease granted/disarmed `140:2506`, Release confirmation `140:3097`, Revoked by another GUI `140:3201` | Wait for agent grant, stay disarmed after grant, explicit ARM before Run; release disarms without automatically restarting ROS; revocation identifies bench-laptop, stops test signals/auto re-run and leaves observation available |
| History and observers (y 4920) | Agent history `138:10938`, Lease available `138:10582`, Monitor observer `141:12188`, Another GUI holds lease `141:12737` | Faults/arm-history drawer with pre-connect Agent history labels versus This connection; nobody/another-GUI ownership; shared live plots with manual commands disabled |
| Checks (y 6560) | Tune/ROS `143:3437`, Monitor observer `143:4234` | Both 1920×1080 and Light. Every other G6 frame was also checked at 1920×1080 in Light; its temporary copy was removed |

**Control rules represented:** a remote connection starts as an observer. It creates its command group only after the lease grant, as required by the host plan's lease rule. ARM is disabled with "ARM needs lease" when this GUI is not the holder; DISARM stays enabled whenever the device link exists, above modal scrims. Taking/releasing a lease never arms automatically. Live edits while another client drives require the agreed warning; opting out lasts only for this connection. Captures and other exclusive operations retain the host plan's disarmed/no-holder exception, rather than treating an observer as a lease holder.

**Responsive checks:** at 1080p the remote Tune gain-history card becomes a compact View history entry, preserving visible gain controls and the live plot. Monitor's three plots share the available height, legends wrap, and plot geometry/axis labels are fitted to the new bounds. This applies to G6; the previously accepted G5 issues below remain open. All G6 frames contain editable layers and G2 instances; the final audit found no image-filled UI nodes and only Inter / JetBrains Mono text. G8 wires the full clickable prototype, including the compact history entry.

## G7 Custom panel

On Custom panels (`2:7`): 11 hi-fi frames at 2560×1440 in Dark, in three-column rows, plus two retained 1920×1080 Light checks. The G4 Calibration slot (`85:4679`) supplies the shell, instance tree and Calibration tab. The panel keeps generic Wiring and Signals context beside it; the tab handles calibration actions, while Params remains the generic editor.

The dashboard shows System, Gyroscope, Accelerometer and Magnetometer levels (0–3), guidance, numeric roll/pitch/yaw and quaternion values, and a simple projected sensor-axis graphic. The sample graphic and numbers use the same pose. Stale telemetry freezes the last pose and marks it stale; before the first sample, the pose is hidden and numbers are dashes. Calibration readiness is independent of arm state.

**Added to G2:** Sensor panels section `160:992`; CalibrationLevel set `160:999` (0, 1, 2, 3, Unavailable) and OrientationPreview set `160:1050` (Live, Stale, Unavailable). Both use existing variables and text styles, have QML descriptions and mapping rows (`160:1132`, `160:1141`), and are shown as instances in Dark (`160:1087`) and Light (`164:1030`). No variables were added.

| Row | Frames | Covers |
|---|---|---|
| Live and snapshot (y 0) | Live calibration `161:1030`, Fully calibrated `161:1344`, Read snapshot and export `161:1647` | Four levels, live pose and numeric values, guidance, Read disabled until all four levels reach 3/3, separate PC export after reading |
| File and apply (y 1640) | Imported file review `161:1965`, Applying `161:2344`, Applied `161:2669` | File source and target name/id, compatibility review, explicit Apply, indeterminate wait for device result, paused/stale sensor telemetry, success only after acknowledgement and fresh data |
| Errors and stale data (y 3280) | File rejected `161:2988`, Apply failed `161:3291`, IMU data unavailable `161:3615` | Incomplete file rejected locally without a send; Busy result with the file retained and explicit retry; no live readings presented when telemetry is stale |
| Review and initial load (y 4920) | Confirm apply `162:2565`, Waiting for first sample `165:3124` | Target device/instance/file, sensor/mounting caution and pause notice before Apply; no-sample state with no invented pose |
| Checks (y 6560) | Live calibration `162:2887`, Confirm apply `163:8006` | Both 1920×1080 Light; all other G7 frames also checked at that size/theme and temporary copies removed |

**File/action rules:** Import validates a file locally and sends nothing. Read from IMU and Export are separate. Apply opens the target-review dialog before sending; retry and apply-again use that same review. The panel shows device operation results and does not claim a PC file was saved to device NVS. The `.cal` extension and metadata are illustrative, pending the host file-format contract. F6 defines the payload and precise sequencing of `calibration_write` / `calibration_apply`; do not infer an atomic combined operation from the one GUI action. A multi-step failure must retain and identify the result of each completed step.

Guidance and the full-calibration prerequisite for reading a profile follow [Bosch BNO055 datasheet revision 1.8, sections 3.11.1–3.11.5](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bno055-ds000.pdf). A profile must match the physical sensor and mounting; module-type validation alone does not establish that its offsets are correct.

**Responsive checks:** the 1080p file card uses tighter spacing and wrapped controls; the generic signal table uses 110/80/fill columns so values and units remain visible. DISARM is enabled with the device link and stays above the confirmation scrim (y 56). All 11 frames use editable layers and G2 instances, only Inter / JetBrains Mono, and no image-filled UI. G8 connects the prototype.

## G7b Loops and sources rework

State: planned on 2026-11-13, nothing built yet. This section is the brief for the session that builds it. The frame ids are in [gui-design-status.md](gui-design-status.md) ("Figma reference"); check that they still exist before use.

### Why

The designs up to G7 assume two fixed loops, `control` and `sensor`, and a per-type list of loop classes. The firmware changed after they were made:
- **F3d (built):** loops are instances of a built-in `loop` type and part of the saved layout. A device has between zero and `CORE_MAX_LOOPS` loops (default 4, at most 8).
- **F4c (planned, not built):** an instance can read a signal of an instance in another loop through a source param.

### What the firmware does now

Read these before building: `docs/protocol/core.md` (the layout family, "Built-in loop and factory layout", the status stream), `docs/protocol/types/loop.md`, and "Configurable loops, the real-time flag and sources" in [firmware-architecture.md](firmware-architecture.md).

**Loops (built):**
- **Identity.** A loop is an instance of type `loop`. Its instance id is the loop id, and its label is its name. Loop ids can have gaps.
- **Placement follow-up (2026-11-14).** Use the device-served enum choices for
  loop placement. Add loop defaults to **Any core**. The Device table shows
  configured placement, not the current CPU. Optional help: "Automatic placement
  is chosen at startup and can change after a reboot." The existing Figma
  screens need this follow-up; this firmware change does not edit them.
- **Its items:**

  | Item | Kind | Behaviour in the GUI |
  |---|---|---|
  | `rate_hz` (1–2000 Hz, default 100) | param | Live. Locked while armed. Sets Unsaved. |
  | `realtime` (bool, default off) | param | Needs a reboot: Pending (violet). |
  | `core` (discovered enum: Core 0, Core 1, Any core) | param | Needs a reboot. New loops default to Any core; factory loops remain on cores 1/0. |
  | `priority` (1–20) | param | Needs a reboot. |
  | `stack_bytes` (3072–16384) | param | Needs a reboot. |
  | `ticks`, `overruns`, `last_run_us`, `max_run_us` | signals | Live statistics. They can be plotted like any signal. |

- **Add, remove, rename:** the same ops as for any instance (`ADD_INSTANCE` with type `loop`, `REMOVE_INSTANCE`, `SET_LABEL`). They are staged, then applied by SAVE and a reboot, and refused while armed.
- **Limits.** One more loop than the device allows is refused with `Capacity`. From firmware F4b the device reports its limits as read-only system params (`limits.loops`, `limits.instances`, and the group limits), so the GUI can show "4 of 4 loops" and disable Add loop beforehand. The refusal stays as the fallback.
- **Remove.** A loop that still holds an instance can't be removed (`Referenced`). An instance that another one is wired to can't be removed either (`Referenced`).
- **Real-time.** A type declares `realtime: required`, `forbidden` or `any`. `GET_TYPES` gives a mask: bit 0 the type may run in a real-time loop, bit 1 in the other loops. The shipped mock actuator and feedback are `required`, the mock IMU is `forbidden`, and no shipped type is `any` yet.
- **Add instance.** The op takes a loop id. `0xFF` picks the lowest-numbered compatible loop. With no compatible loop it answers `LoopMismatch`.
- **Paused actions** are refused while armed on instances in any real-time loop, not only in `control`.
- **Wiring** still needs both instances in the same loop (`LoopMismatch`).
- **Factory layout.** A blank device has `control` and `sensor` and no module instance. A saved layout may hold zero loops.
- **Safe mode** starts no loop. The configured loops are listed as offline.
- **A loop that fails to start** is Failed, and its instances are `DependencyMissing`. The other loops keep running.
- **Where the numbers come from.** `GET_INFO` lists the running loops (id, name, rate, real-time flag). The status stream carries `loop_id, overruns` per running loop. Run times come from the loop's signals. CPU load is per core, not per loop.

**Sources (planned, F4c):**
- A source is a param whose value names one signal (or input) of another instance, in any loop. 0 means not connected. It needs a reboot.
- The consumer gets a sampled copy with `value`, `age_us`, `valid` and `fresh`.
- A missing producer does not stop the consumer: it runs and sees `valid == false`.
- A target that does not exist or has the wrong value type is refused (`BadRef`). An instance that a source points at can't be removed (`Referenced`).
- **How the GUI reads a source's state** (settled 2026-11-13, firmware plan "What the host sees"): a source is three items. The param holds the target and carries a `Source` flag. Two signals carry the state: `<name>.value` (the sampled copy) and `<name>.age_us` (its age in µs). The GUI reads them like any live signal.
  - `age_us` of `0xFFFFFFFF` means no valid sample. The GUI then says why: the param is 0 (not connected), the target instance is not running (missing), or no sample has arrived yet.
  - Stale is the GUI's rule: an age above three periods of the producer's loop, unless the consumer's type has its own limit param.
  - The age can be plotted on Monitor like any signal.

### Decided

The six rows of 2026-11-13 in the Decisions table: loops are edited on Layout; Remove follows the firmware; Loop at add; sources are designed now as provisional; the sample device has three loops; limits and source state.

### To ask the user before building

Ask these in the Discuss stage, with the recommendation first. Record the answers in the Decisions table.

1. **The Add control on the Instances card.** Recommended: one Add button with a menu (Instance, Loop). Alternative: two buttons.
2. **What a loop header shows in the tree.** Recommended: name, rate, a real-time badge and the budget % (run time over the period). The current design shows "CPU load" there, which the device does not report per loop.
3. **The frame "After remove, wiring invalid" (`89:5729`).** The state can no longer be reached by a remove. Recommended: turn it into "Layout invalid at boot" (an entry that fails validation at boot is listed as invalid, and ARM is blocked), which keeps the Invalid RefPicker and the banner in use. Alternative: delete it.
4. **The wiring picker's disabled entry (`85:4111`).** It shows `sim_motor` pending in the `sensor` loop, which the mock types no longer allow (they need a real-time loop). Recommended: remove that entry from the frame and keep the disabled-with-reason state in the G2 MenuItem preview. Alternative: give the frame a second real-time loop.

### Work list

Build in this order. Each part can be its own session; the state table in the status file says how far it is.

**A. Design system (page G2 `2:4`)**
- **TreeItem `36:347`:** the loop header becomes a selectable loop item. Variants: default, hover, selected, pending add, pending change, pending remove, failed, offline. It shows the name, the rate, a real-time badge and the budget %.
- **Real-time badge:** use Badge `33:189` with the text "Real-time" in a neutral tone. Add a tone only if none fits. It must not look like the arm state.
- **Source row and picker (provisional):** a row made of the name, a signal picker and the sampled value with its age. States: not connected, valid, stale, missing, pending, refused. The picker lists instances, then their signals of the right value type, with the loop and rate in the detail line (MenuItem `Show detail`). Build the picker from RefPicker `32:169` if it fits, else add a SourcePicker set. Mark both "provisional (F4c)" in their descriptions.
- **Add menu:** per question 1. The Instances card also shows the counts against the limits ("3 of 4 loops", and the same for instances).
- Add a QML mapping row for every new set, and check the section bounds afterwards (the mapping table has overlapped Icons before).

**B. New Layout frames (page Screens – Local `2:5`)**

Put them under a new header "G7b · Loops and sources", below the G5 checks row. Clone Instance detail `84:3890`.

| Frame | Shows |
|---|---|
| Loop detail (`control`) | The loop selected in the tree. Header: name, "loop · real-time · running", Rename, Remove loop. Params: `rate_hz` live; `realtime`, `core`, `priority`, `stack_bytes` marked as needing a reboot. Right column: "Instances in this loop" and live Statistics (ticks, overruns, last and longest run, budget). No Wiring or Actions card. |
| Loop detail, pending and unsaved (`fusion`) | `priority` changed: Pending (violet), old → new. `rate_hz` changed: applied live, Unsaved. The pending banner. |
| Loop detail while armed | Rename, Remove and the reboot params disabled with a tooltip. `rate_hz` Locked. Statistics still live. |
| Add loop | The dialog: label, rate, real-time switch, core, priority, stack, with the defaults filled in. A note that it takes effect after SAVE and a reboot. |
| Add loop at the limit | Add loop disabled in the menu with "4 of 4 loops". A second state: the dialog with a `Capacity` refusal next to Add, for a device that does not report its limits. |
| Remove refused: loop in use | `fusion` selected; Remove loop disabled; the tooltip names `odom`. |
| Remove refused: wired from | `left_pwm` selected; Remove instance disabled; the tooltip names `left_drive` and its `actuator` wiring. Replaces "Remove with dependents" `89:5301`. |
| Loop failed to start | `fusion` Failed and `odom` `DependencyMissing` in the tree; the failure card with Open faults; ARM disabled with the reason. |
| Add instance, loop choice | Type list with each type's need ("needs a real-time loop", "any loop", "not in a real-time loop"). `fusion.odometry` picked (an `any` type); the Loop combo lists all three loops with rate and real-time, the device's default first. |
| Add instance, no compatible loop | A `required` type on a layout with no real-time loop: the reason, and an Add loop link. |
| No loops | A saved layout with zero loops: the empty tree and a banner that leads to Add loop. |
| Sources (`odom`, provisional) | A Sources card under Wiring: `left` valid (value, age 0.4 ms), `heading` stale (amber, age 2.1 s, "sensor loop paused"), `right` missing ("right_enc is not running"). |
| Source picker open | Instances, then signals; other loops are allowed; entries of the wrong value type are left out. |

**C. Existing Layout frames to change (G4, y 10040 to 14960)**
- **All of them:** the tree gets the `fusion` loop and `odom`, and the loop headers use the new TreeItem variants.
- **Wiring picker open `85:4111`:** per question 4.
- **Remove with dependents `89:5301`:** delete it once its replacement in B exists.
- **After remove, wiring invalid `89:5729`:** per question 3.
- **First boot, Add instance `89:6715`:** the tree is no longer empty. It shows the two factory loops with no instances; the dialog is the reworked Add instance.
- **Armed, layout locked `85:5034`:** check the tooltip wording ("real-time loop", not "control loop").
- **The checks `89:7040` and `89:7326`:** rebuild them from the changed frames.

**D. Device (G3 on `2:5`, and its G6 copies on `2:6`)**
- **The loop table:** one row per running loop, keyed by loop id. Columns: Loop, Rate, Real-time, Core, Priority, Run time (last / longest), Budget, Overruns. A row links to the loop on Layout. It is read-only.
- **Safe mode `70:2136`:** "No loop is running in safe mode", with the configured loops listed as offline.
- **Every other Device frame, its checks, and every G6 frame cloned from Device:** the third row and the new columns. Find the table by its card name in each frame; don't rely on a list of ids.
- **ARM refused `69:1729`:** check that the "instance failed" reason can name a loop.

**E. Monitor and the custom panel**
- **Monitor (`109:8621`, `109:9147`, `109:9731`, check `116:9143`, G6 observer `141:12188`, `143:4234`):** the signal browser lists three loops in its rates block, and the loop instances appear as signal owners with their statistics. No layout change.
- **Tune:** no change. It has no tree and no loop table.
- **Calibration (G7, page `2:7`):** the tree gets `fusion` and `odom` on all 11 frames and the 2 checks. The IMU runs in `sensor`, which is not real-time, so its paused calibration actions are not refused while armed; check that no text says they are.

**F. Records outside the screens**
- **FigJam flow 5** (first-boot layout): the first boot now has two loops. Change the text of the first step.
- **G1 wireframes:** leave them as they are. They are the record of that phase.
- **Cover:** add a G7b row next to the phase labels.

**G. Checks and record**
- Check every new or changed frame at 1920×1080 and in Light. Keep two copies as the record: Loop detail at 1920×1080, and Sources in Light.
- Then follow the skill's Record and Finish stages.

### Rules to keep while reworking

- Pending (needs a reboot) is violet and never looks live. A loop's `rate_hz` is live; its other four params are pending.
- A refused op is not staged, and the reason is shown next to it, naming the instance or loop in the way.
- Anything refused while armed is disabled while armed and says why.
- Loop names come from the device. No text or layout may assume the names `control` and `sensor`, or that there are two loops.
- Don't put the real-time badge next to the arm state, and don't give it the Armed amber.

### Open issues from this plan

- **Both firmware gaps this plan found are settled** (2026-11-13) and scheduled in [overhaul-status.md](overhaul-status.md): the limits come with F4b, the source signals with F4c. Neither is built yet, so the frames that show them are ahead of the firmware.
- **Sources are provisional** until F4c is merged: recheck the source row, the picker and the sample `fusion.odometry` items then.

## Open issues

- **Rename and the firmware to-do:** the Rename dialog assumes `SET_NAME` applies at once and sets `nvs_dirty`, so SAVE writes it (one of the open questions in [todo-device-name.md](todo-device-name.md)).
- **G3 notes for H2:** loop overruns and CPU per core come from the status stream; the "Budget" column (run time over the period) is computed on the host. The reboot notice needs the GUI to remember which params were changed since the last save.
- **Device name (firmware to-do):** `GET_INFO` carries no device name or id yet, so until the firmware adds them the GUI lists recent devices by port and firmware. The to-do for the firmware side is [todo-device-name.md](todo-device-name.md). From G2 on, the designs show the name and id (status bar, Connect's recent list, Device info with Rename).
- Run metrics on Tune (rise time, overshoot, settling, steady error) are computed on the host. That is a small addition to `libdrivelink`'s plant-identification math (H1).
- G2 details noted while wireframing (done in G2): the Arm button has a disabled state for Fault and safe mode; the plot frame has real traces and a legend.
- `series/2` (orange) is close to Armed amber: fine inside plots, but never next to the arm state (G5 check). The Unsaved badge contrast in Light was fixed in G3.
- **Layout ops:** closed. The firmware has the loop argument on `ADD_INSTANCE` and refuses a remove with dependents (`Referenced`); G7b brings the G4 frames in line. [todo-layout-ops.md](archive/todo-layout-ops.md) records the outcome.
- **G7b notes for H2 (so far):** a loop's run time, longest run and overruns are signals of its loop instance; the status stream carries only the overrun counts, each with its loop id. "Budget" stays a host calculation (run time over the period). Loop ids can have gaps, so key everything by id, not by position.
- **G4 notes for H2:** the GUI keeps the pending changes list (GET_PENDING plus the layout ops it staged) to fill the Review popover; a layout op refused by the device is not staged, so the row keeps the typed value with the reason until the user edits it or presses Esc (which also disarms). `color/text/tertiary` is 3.8:1 on surface in Light, so small caps group headers use `color/text/secondary`.
- **Status bar faults badge:** the G3 Device frames show "2 faults" (history) next to a Faults card with "0 active"; decide in G8 whether the badge counts active faults only.
- Tabular numerals can't be set through Figma's plugin API, so the numeric styles only say so in their descriptions; QML sets `font.features: { "tnum": 1 }`.
- **G5 at 1920×1080:** the Tune plot gets the narrowest column (the 2×2 gains grid stays 756 px), the A/B compare card overflows (stack the gain differences above the table) and Monitor's paused toolbar overflows. To settle in H2 (for example gain history as a tab, or one column of gains with scrolling).
- **G5 notes for H2:** slider ticks in the Figma frames are only approximate (marker positions inside an instance can't be moved); in QML each tick sits at its value on the log scale. CheckItem has no warning state; Plant identification shows its open-loop caution with a neutral icon and "warning".
- **G6 notes for H3:** reset the live-edit warning opt-out on reconnect; while confirmation is open, keep the proposed gain local and send nothing until Apply live. A lease-revoked event cancels auto re-run and any pending command/test start. The takeover checkbox is unchecked by default. Agent history is an illustrative recent-event sample; its retention limit and transport details remain part of H3. The full prototype and lease-chip navigation are connected in G8.
- **G7 notes for F6/H2/H3:** F6 defines calibration signal/action descriptors, payload, paused-action sequencing and persistence. H2 defines the PC file format, physical-sensor provenance, Euler convention and host conversion, result handling and cancellation/reconnect behavior. Map device errors verbatim; a timeout is not proof that an apply did not occur. Before using this panel remotely, H3 must settle calibration-action authorization while another client owns the lease; the live-param permission does not automatically authorize sensor reconfiguration. This phase changes no firmware, protocol or safety policy.
