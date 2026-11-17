# drivelink-gui

The PC GUI: Qt 6 and QML, built from the Figma design
([gui-design.md](../../plans/gui-design.md), node ids in
[gui-design-status.md](../../plans/gui-design-status.md)). Windows uses Qt
6.12.0 with Qt's MinGW 13.1; Qt 6.10 (Ubuntu 26.04) is the API floor that the
Linux build enforces. The superbuild builds it unless `DRIVELINK_BUILD_GUI=OFF`
(the `linux-no-gui` preset). colcon ignores this directory.

H2a-1 provides the style layer; H2a-2 adds the application, local device
backend and shell; H2b-1 adds Connect and Device, H2b-2 the firmware update
and H2c-1/H2c-2 Layout (below). Tune and Monitor are placeholders until their steps
([host-steps.md](../../plans/host-steps.md)).

| Path | What |
|---|---|
| `theme/` | `tokens.json` (Figma export), `gen_theme.py`, generated `Theme.qml`, `export_tokens.js` |
| `resources/` | Inter, JetBrains Mono, the Lucide icons, their licences ([SOURCES.md](resources/SOURCES.md)) |
| `style/` | QML module `DriveLink.Style` (static library `drivelink_gui_style`) |
| `gallery/` | `drivelink-style-gallery`: every control state, Dark and Light |
| `backend/` | `Connection`, `LocalConnection`, `SafetyKeys`, generic Qt models and command planning |
| `app/` | `drivelink-gui`, static QML module `DriveLink.Shell`, screenshot mode |
| `tests/` | `gui-quick`, `gui-gallery`, real-vdev `gui-backend`, `gui-screenshots`, scenario script |

## Application and backend

Run `drivelink-gui`, or pass `--endpoint tcp://127.0.0.1:8765` (vdev) or
`--endpoint serial://COM3`. Windows needs Qt's MinGW and Qt `bin` directories
on PATH until H2g packages the application. The Windows executable is
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

**Keepalives leave input values unchanged.** Handles have empty descriptor/hold
vectors and send prefixes only; this backend exposes no value-command API.
Descriptor defaults are not semantic holds. H2d must install explicit finite
holds and full descriptors before introducing manual value commands.
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
  `DriveLink/Modules/<type name>/Panel.qml` is in the resources; it is loaded
  with `backend` and `nodeKey`. None ships until H2f.

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
`layout-add-loop` (with `--select TYPE:INSTANCE` for the Layout screens) and, with `--image FILE`, `update` (the
image choice), `update-writing`, `update-confirmed`, `update-rolledback`;
`--expect-state` accepts `ready`,
`fault`, `safe`, `lost`. Except for Connect, pass an endpoint. `--settle MS`
waits before capturing (loop run time needs about a second); `--settings FILE`
isolates recent devices. It does not inject faults or provision a supplied
device; `unsaved` and `notice` rename (and reboot) the device, so they accept
only a `tcp://` vdev endpoint, as do the `update-*` screens (they install
firmware).

From the repo root, the isolated Windows vdev campaign is:

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.12.0\mingw_64\bin;' + $env:PATH
python host/gui/tests/scenarios.py --gui host/superbuild/build/windows-mingw/gui/drivelink-gui.exe --vdev host/superbuild/build/windows-mingw/vdev/drivelink-vdev.exe --backend host/superbuild/build/windows-mingw/gui/gui_backend_tests.exe --out build/gui-scenarios
```

The script defaults to offscreen/software rendering, runs the backend checks
when `--backend` is supplied, and renders twenty-three scenarios, the last four the firmware update screens (it
writes a non-executable vdev image for them), into separate folders (92 PNGs,
dimensions checked). The six Layout scenarios first build the G7b three-loop
sample on vdev with `drivelink-cli` (next to vdev, or `--cli`), save and
reboot it, and stage changes for the pending ones; `--only NAME...` picks some. `--smoke`
renders only Connect and Device;
CTest uses that subset on both OSes. Use the CTest-resolved executable paths
on Linux (`ctest --preset linux -N -V -R '^gui-'` from `host/superbuild`).
