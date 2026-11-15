# drivelink-gui

The PC GUI: Qt 6 and QML, built from the Figma design
([gui-design.md](../../plans/gui-design.md), node ids in
[gui-design-status.md](../../plans/gui-design-status.md)). Windows uses Qt
6.12.0 with Qt's MinGW 13.1; Qt 6.10 (Ubuntu 26.04) is the API floor that the
Linux build enforces. The superbuild builds it unless `DRIVELINK_BUILD_GUI=OFF`
(the `linux-no-gui` preset). colcon ignores this directory.

H2a-1 provides the style layer; H2a-2 adds the application, local device
backend and shell. Connect currently takes an endpoint; Device, Layout, Tune
and Monitor contain shell placeholders. Their content follows in H2b onward
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
the underlying page and keep DISARM live. The faults drawer is a frame for
H2b. Toasts stack to four; Info/Ok expire and Fault remains dismissible.
Dirty/pending flags are bound; `DLStatusBar.configActionsVisible` stays false
until H2b supplies SAVE/REVERT workflows and the faults-badge policy.

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
| Dialog | `DLDialog` | Default / `destructive`; optional confirmation checkbox; Enter accepts only a Default dialog |
| Toast | `DLToast` | Info and Ok dismiss after 4 s; Fault stays |
| Tooltip | `DLToolTip` | 500 ms delay, wraps at 320 px |
| Icon/* | `DLIcon` | a bundled Lucide icon by name, recoloured |

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
Native arming fixtures SAVE their 10 Hz loop rates and 1 s watchdog so a
reboot retains the desktop scheduling allowance.

## Shell screenshots

The app's `--out DIR` writes four PNGs (2560×1440 and 1920×1080, Dark and
Light), waits for live state with a timeout, then disconnects. `--screen`
accepts `connect`, `device`, `layout`, `tune`, `monitor`, `drawer`, `dialog`;
`--expect-state` accepts `ready`, `fault`, `safe`, `lost`. Except for Connect,
pass an endpoint. It does not inject faults or provision a supplied device.

From the repo root, the isolated Windows vdev campaign is:

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.12.0\mingw_64\bin;' + $env:PATH
python host/gui/tests/scenarios.py --gui host/superbuild/build/windows-mingw/gui/drivelink-gui.exe --vdev host/superbuild/build/windows-mingw/vdev/drivelink-vdev.exe --backend host/superbuild/build/windows-mingw/gui/gui_backend_tests.exe --out build/gui-scenarios
```

The script defaults to offscreen/software rendering, runs the backend checks
when `--backend` is supplied, and renders ten scenarios into separate folders
(40 PNGs, dimensions checked). `--smoke` renders only Connect and Device;
CTest uses that subset on both OSes. Use the CTest-resolved executable paths
on Linux (`ctest --preset linux -N -V -R '^gui-'` from `host/superbuild`).
