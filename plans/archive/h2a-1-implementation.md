# H2a-1 GUI build, Theme and style

Status: approved by the user 2026-11-15, built; results at the end. Step brief: "H2a GUI foundation"
in [host-steps.md](../host-steps.md). Branch `claude/overhaul-H2a-1` from
`origin/host_overhaul`; PR into `host_overhaul`.

## User decisions (2026-11-15, before the plan)

- **Split H2a** into H2a-1 (this step: build, CI, Theme, style, tests) and
  H2a-2 (next session: shell, DISARM/Esc, backend, command groups, item-model
  bases, vdev scenario hooks, screenshot tool). H2a-2 keeps H2a's independent
  review and vdev check; H2b depends on H2a-2.
- **WSL packages:** the session runs `tools/setup_linux.sh` as root in
  `Ubuntu-26.04` once the script exists.
- **Fonts and icons:** downloaded at pinned versions and committed with their
  licences and SHA256s (no network at build time).
- **Figma:** read-only in this step (token export). LP's placement enum is not
  applied to the frames; H2c builds it in code.

## Scope

In: the GUI's build and CI, `tools/setup_linux.sh`, the G8 token export and the
`Theme` generator, bundled fonts/icons, the 13 shell controls of the DriveLink
style, a style gallery that renders every control state to PNG, Qt Quick Test
in both host checks, the G8 issue assignment.

Out (H2a-2): the `drivelink-gui` application, shell layout, Esc/DISARM wiring,
connection/Qt adapters, arm/ack, the GUI command groups, item-model bases, vdev
scenario hooks and the screens screenshot tool. The controls here only expose
properties and emit signals; nothing in H2a-1 talks to a device.

Estimated size: ~3k hand-written lines plus ~1.5k generated (tokens.json,
Theme.qml) and binary resources.

## Changes

- **Superbuild.** `DRIVELINK_BUILD_GUI` option: ON in `linux` and
  `windows-mingw`, OFF in `linux-no-gui`. `windows-mingw` gains
  `CMAKE_PREFIX_PATH=C:/Qt/6.12.0/mingw_64`. `find_package(Qt6 6.10 REQUIRED
  COMPONENTS Quick QuickControls2 QuickTest Svg)`, `qt_standard_project_setup(REQUIRES 6.10)`.
  `-Werror` stays on for our sources (Qt headers as SYSTEM).
- **`host/gui/`** (`COLCON_IGNORE`, README):
  - `theme/tokens.json`: every Figma variable (Primitives, Color Dark/Light,
    Size, Type), the 11 text styles and 2 effect styles, aliases resolved, each
    with its QML name from the variable description. Exported by one read-only
    `use_figma` script kept as `theme/export_tokens.js` with the steps in the README.
  - `theme/gen_theme.py`: tokens.json → `Theme.qml` (`pragma Singleton`,
    `property bool dark`, colour/space/radius/size/type/font properties,
    text-style font objects with `tnum` on numeric styles). Generated file is
    committed; `--check` fails when stale. Pure Python, unit-tested.
  - `resources/`: Inter 4.1 (static Regular/Medium/SemiBold as G2's text styles
    need), JetBrains Mono 2.304 (Regular/Medium), the Lucide 0.460.0 SVGs that
    G2 uses (`Icon/<name>` components, ~67), OFL/ISC licence files and a
    `SOURCES.md` with URLs and SHA256s. Compiled into Qt resources.
  - `style/` QML module `DriveLink.Style` (static plugin), built on
    `QtQuick.Templates` with Basic as fallback, every value from `Theme`:
    DLButton (4 styles × states), DLIconButton (2 sizes), DLArmButton (Arm,
    Arming, Disarm, Disarm disabled, Ack fault, Arm disabled; signals only),
    DLStateChip (Offline … Safe mode), DLBadge (8 tones × 2 sizes),
    DLDeviceChip (Serial/Agent, name or id), DLNavItem, DLNavRail, DLStatusBar
    (three zones; centre group stays centred; Unsaved/Pending/Faults flags and
    inline Save/Revert slots), DLBanner (Info/Pending/Armed/Fault, up to 3
    buttons), DLDialog (Default/Destructive, optional confirmation checkbox),
    DLToast (3), DLToolTip, plus DLIcon (recoloured Lucide SVG). Accessible
    names on every control; tabular numerals on numeric text.
  - `gallery/`: `drivelink-style-gallery` renders each control in each state,
    dark and light, offscreen to PNG (`--out DIR`). Used to compare with the G2
    component sets in Figma; H2a-2 grows it into the screens screenshot tool.
  - `tests/`: Qt Quick Test (`QUICK_TEST_MAIN`, offscreen) as ctest
    `gui-quick`; a ctest `gui-gallery` that runs the gallery and checks every
    PNG was written.
- **`tools/setup_linux.sh`**: the one Linux package list, idempotent
  (skips installed packages), profiles `base` (toolchain, Python/PyYAML) and
  `host` (base + Ubuntu's Qt 6.10 dev/QML modules). Run as root.
- **`tools/validate.py`**: `host-linux` builds the `linux` preset (with GUI);
  its missing-tool message names `setup_linux.sh`. `gen` also runs
  `gen_theme.py --check` and its tests.
- **CI:** host-linux and native-tests call `setup_linux.sh` (no package list in
  `ci.yml`); host-windows already installs Qt 6.12.0 and gains nothing but the
  GUI build through `host-win`.

## Tests

- `gen_theme.py` unit tests: alias resolution, both modes complete, QML name
  derivation, deterministic output, `--check` on a stale file.
- Quick Test: every control instantiates in both themes; theme switch changes
  bound colours; ArmButton variant/enabled logic and its signals (Disarm
  disabled only without a link); Button/IconButton disabled and pressed states;
  Badge tones; StatusBar centre group stays centred across widths and shows the
  flags; Banner button visibility; Dialog accept/reject; fonts resolve to Inter
  and JetBrains Mono; numeric text has `tnum`; every icon loads.
- `gui-gallery` smoke on both OSes.

## Hardware check

None: H2a-1 has no device code. The brief's vdev check moves to H2a-2.
Visual check: gallery PNGs against Figma G2 screenshots (read-only), dark and light.

## Review

Self-review at high effort: build, style and tests only, no arming, DISARM,
command or threading path (those are in H2a-2 with the independent review).

## Deviations / internal choices (logged)

- Theme.qml is generated and committed (like `protocol/` generated headers), checked by `gen`.
- Controls built on `QtQuick.Templates`; icons recoloured via Qt Quick Controls' icon colour path.
- `setup_linux.sh` profiles so native-tests CI need not install Qt.
- No wire, safety rule or later-step change beyond the approved split.

## Docs

`host/gui/README.md` (new), `host/README.md` (GUI build), `host/CLAUDE.md`
(GUI rules: generated Theme, tokens only, generic), `docs/development.md`
(setup_linux.sh, host-linux preset, gen theme check), `plans/host-steps.md`
(H2a split into H2a-1/H2a-2), `plans/host-status.md`, `plans/gui-design.md`
and `plans/gui-design-status.md` (G8 done: issue assignment, tokens path).

## Results

- **Built as planned.** `tokens.json` holds 154 variables (Primitives 64,
  Color 51, Size 25, Type 14), 11 text styles and 2 effect styles; `Theme.qml`
  is generated. 67 Lucide icons and five font files are bundled.
  `DriveLink.Style` has the 13 shell controls plus `DLIcon`, `DLShadow` and
  `DLReasonHover` (added: a disabled control gets no hover, so the wrapper
  shows why it is disabled).
- **Internal choices** (logged in the status file): the reviewed rename
  `borderFocusWidth`; non-modal `DLDialog`; shadows without shader effects and
  software rendering offscreen; `NO_PLUGIN` static QML module; `setup_linux.sh`
  `--check` and profiles, git-only CI bootstrap, `.gitattributes` for `*.sh`.
- **Tests:** 41 Qt Quick tests (Theme, controls, status bar) and the gallery
  smoke pass on Qt 6.12.0/MinGW 13.1 and Ubuntu's Qt 6.10 (WSL); 10 generator
  unit tests run in `gen`.
- **Visual check:** the gallery's Dark rendering compared with the Figma G2
  component dumps and the StatusBar set screenshot: sizes, paddings, colours,
  fonts and states match. Hover is not rendered offscreen.
- **Hardware:** none (no device code).
- **Review:** self-review at high effort; no findings beyond the fixes made
  while testing (Dialog implicit size, test timing before polish).
