# drivelink-gui

The PC GUI: Qt 6 and QML, built from the Figma design
([gui-design.md](../../plans/gui-design.md), node ids in
[gui-design-status.md](../../plans/gui-design-status.md)). Windows uses Qt
6.12.0 with Qt's MinGW 13.1; Qt 6.10 (Ubuntu 26.04) is the API floor that the
Linux build enforces. The superbuild builds it unless `DRIVELINK_BUILD_GUI=OFF`
(the `linux-no-gui` preset). colcon ignores this directory.

H2a-1 provides the style layer: the `Theme`, the bundled fonts and icons, the
shell controls, a gallery and the tests. The application, shell, device backend
and screens follow from H2a-2 ([host-steps.md](../../plans/host-steps.md)).

| Path | What |
|---|---|
| `theme/` | `tokens.json` (Figma export), `gen_theme.py`, generated `Theme.qml`, `export_tokens.js` |
| `resources/` | Inter, JetBrains Mono, the Lucide icons, their licences ([SOURCES.md](resources/SOURCES.md)) |
| `style/` | QML module `DriveLink.Style` (static library `drivelink_gui_style`) |
| `gallery/` | `drivelink-style-gallery`: every control state, Dark and Light |
| `tests/` | Qt Quick Test (`gui-quick`) and the gallery smoke (`gui-gallery`) |

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

Rules the controls keep, and the shell (H2a-2) completes:

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
