# GUI design status

The working state of the DriveLink GUI design in Figma, so a new session can pick up where the last one stopped. Start a session with `/gui-design-step`.

The plan, the decisions and the phase contents are in [gui-design.md](gui-design.md). This file holds only the state: where things stand, what comes next, and what the next session must know.

Last updated: 2026-11-14 (six G7b choices recorded; plan aligned with built firmware; awaiting scope approval; G7 remains in review).

## Where things stand

| Phase | State | Notes |
|---|---|---|
| G0 Foundations | done | FigJam screen map and flows 1–11; state inventory in gui-design.md |
| G1 Wireframes | done | 21 frames. Tune (12) and Monitor (14) updated after G2 with the tuning controls |
| G2 Design system | done | Accepted 2026-11-12. Extensions through G6 accepted; G7 CalibrationLevel and OrientationPreview reviewed with G7 |
| G3 Shell, Connect and Device | done | Accepted 2026-11-12. 15 frames on Screens – Local plus 4 kept check copies |
| G4 Layout | done | Accepted 2026-11-12. 11 frames on Screens – Local plus 2 kept check copies |
| G5 Tune and Monitor | done | Accepted 2026-11-13, including Auto re-run default. 9 frames plus 2 kept check copies; documented 1080p issues remain open |
| G6 Remote mode | done | Accepted 2026-11-13, including the four G2 extensions. 16 frames plus 2 retained 1080p Light checks |
| G7 Custom panel | in review | 11 Calibration screens on Custom panels, 2 retained 1080p Light checks; awaiting acceptance |
| G7b Loops and sources rework | planned | Discussion complete 2026-11-14; revised Parts A–G await scope approval. 17 new Layout frames planned; nothing built. Current sources use shipped fusion.mock |
| G8 Prototype and handoff | planned | After G7b |

States: `planned`, `in progress`, `in review` (built, waiting for the user's acceptance), `done`.

## Next step

1. Obtain agreement to the revised **G7b** scope in [gui-design.md](gui-design.md), "Discussion outcome and scope approval". All six questions have answers; do not ask them again. G7 remains in review by explicit user choice on 2026-11-14.
2. After scope approval, build Parts A–G in order: A design system, B 17 new Layout frames, C existing Layout frames, D Device and remote copies, E Monitor and custom-panel trees, F FigJam/Cover records, G checks and record. No Figma edits have been made during planning. A session may stop after any part; record completed parts and IDs.
3. Review **G7 Custom panel** separately before G8, including its G2 components and reconciliation with built F6 calibration actions/persistence. G7b's shared-tree update does not accept G7.
4. After G7 and G7b are accepted, discuss **G8 Prototype and handoff**: key G0 flows to wire, the status-bar fault count, responsive behavior (including the known G5 issues) and the H2/H3 handoff scope before building.

## Handoff note

- **Branch:** this session uses local `codex/gui-design-g7b` in worktree `86ff`, based on `0f70c6a` (completed firmware overhaul, also the fetched `origin/firmware_overhaul`). Publish with `git push origin HEAD:Codex/gui-design` per the invoked skill; no PR unless the user requests it. The local `Codex/gui-design` branch is checked out in worktree `7805`, so it was not moved or edited. Its fetched remote tip was verified to be an ancestor of this base; publication is a normal fast-forward. The old instruction to use `claude/gui-loop-rework` is superseded. Never target `main`.
- **G7b in one paragraph:** loops, limits and sources are built. Edit loops as instances on Layout; remove with dependents is refused. Use one Add menu and selectable loop headers with name, rate, neutral Real-time badge and budget %. The baseline has `control`, `sensor` and `fusion`, with `fusion.mock` labelled `odom` and its actual two f32 sources. The wiring-picker scenario adds real-time `test` with `sim_motor` to demonstrate cross-loop refusal. Delete the obsolete after-remove-invalid frame. The revised plan covers Parts A–G and 17 new Layout frames; approval of this scope is the next gate.
- **G7b traps:**
  - Retained normal frames with a tree or Device loop table use the three-loop baseline: G4, G7, Device and its G6 copies. Preserve explicit scenario differences: four loops for wiring refusal, factory loops for first boot, no loops for the saved-empty layout, offline/failed states where appropriate. Update in one scripted pass per page, then screenshot each frame.
  - The G4 Instance detail tree (`84:4122`) was checked on 2026-11-14: it is a FRAME containing separate G2 TreeItem INSTANCE rows, not a shared tree component. The Device table card `67:1267` was located; inspect its row structure before editing it. Do not assume a component update adds the third loop to every frame.
  - Loop names and the loop count come from the device. Nothing may assume `control` and `sensor`.
  - The mock actuator and feedback need a real-time loop. In wiring picker `85:4111`, move the scenario's `sim_motor` into second real-time loop `test`; its cross-loop entry stays disabled with a reason.
  - Read `docs/protocol/core.md`, `docs/protocol/types/loop.md` and `docs/protocol/types/fusion.mock.md`. The source target picker accepts signals and inputs of the required value type; `left_enc.count` is i32 and cannot feed fusion.mock's f32 feedback source. Use `left_enc.position` instead.
  - Source validity is derived from the age sentinel and discovered instance state; `fresh` is not a wire signal. Hide invalid values, distinguish telemetry unavailable from no valid sample, and do not infer that a producer is paused from age alone. Pending targets must not relabel current live values.
- **Scope rules:** design only. No QML or C++. Don't touch `firmware/`, `protocol/` or `docs/protocol/`. Firmware needs found while designing go into a `plans/todo-*.md` file (like [todo-device-name.md](todo-device-name.md)) and a line in the Notes of [overhaul-status.md](overhaul-status.md), after asking the user.
- **Things to check in G5** (from G2): plot series 2 (orange) is close to Armed amber, so never put it next to the arm state. The Light Unsaved contrast was fixed in G3.
- **G5 rules to keep:** run colours are fixed per run; Run is disabled with "Arm first" while disarmed (no "Arm and run"); Auto re-run waits for a first manual Run; Tune has no Save of its own (status bar), only "Revert gains to saved"; a refused plot add says why next to the signal and offers a lower rate.
- **G4 rules to keep:** pending (reboot-required) is violet and never looks live; a refused layout op is not staged and its row says why, naming the clashing instance; ARM blocked by the layout shows as ArmButton "Arm disabled" with the reason in the status bar, not as Fault; there is no per-change undo, only Discard all.
- **Firmware to-dos from the design:** [todo-device-name.md](todo-device-name.md) (G1) is open and in the Notes of overhaul-status.md. [todo-layout-ops.md](archive/todo-layout-ops.md) (G4) is closed: the firmware has the loop at `ADD_INSTANCE` and refuses a remove with dependents.
- **G3 rules to keep:** the scrim of a modal starts under the status bar (DISARM stays live); DISARM is enabled whenever there is a link; refused-while-armed buttons are disabled with a Tooltip saying why.
- **Rename** assumes `SET_NAME` applies at once and sets `nvs_dirty` (an open question in todo-device-name.md; not yet put to the firmware side).
- **G5 accepted:** gain of 0 and negative gains follow the Decisions table; Auto re-run on by default was explicitly accepted 2026-11-13. Keep the documented 1080p issues open for handoff.
- **G6 rules:** remote connections observe first and create their command group only after a lease grant (host plan clarified). Lease ownership is separate from arm state. ARM needs the lease; DISARM stays available with a device link, above every scrim. A grant leaves the board disarmed; revocation stops auto re-run/test starts. Live edits confirm before sending while another client drives; opt-out lasts one connection. Device holds the Clients and lease card; the chip navigates there in G8.
- **G6 at 1080p:** Tune gain history uses a compact View history entry. Monitor legends wrap, its three plots share height, and the axes are fitted without shrinking the text. These fixes are scoped to G6; G5's accepted 1080p issues remain open. All 16 G6 frames checked at 1920×1080 in Light; only Tune/ROS and Monitor/observer copies kept.
- **Naming:** the manual-command toggle is **Latch**, not Hold, because "hold" already means an input left at its hold value (Remove, Hold all).
- **G7 rules and unresolved reconciliation:** the existing design uses local import, explicit target-reviewed Apply, separate Read / Export and stale/no-data handling. A level of 3 is not an ARM permission. F6 is now built and persists typed calibration through explicit SAVE; the older G7 statement that SAVE is not calibration persistence must be reconciled during G7 review. G7b changes the tree only. H3 remote calibration-action permissions remain open; do not infer them from live-param permission.
- **G7 at 1080p:** wrapped file controls, compact metadata/activity spacing and 110/80/fill generic signal columns. All 11 screens checked in Light at 1920×1080; only Live calibration and Confirm apply retained. G2 review instances also checked in both themes.

## Figma reference

Team `drivelink` (Education plan). The MCP seat is a View seat, but `use_figma` can write and `node.screenshot()` inside `use_figma` works for visual checks.

- **Design file:** `49NRHfKe8rOoVlloLLCqLh` ([link](https://www.figma.com/design/49NRHfKe8rOoVlloLLCqLh))
  - Pages: Cover `0:1`, G0 Foundations `2:2`, G1 Wireframes `2:3`, G2 Design system `2:4`, Screens – Local `2:5`, Screens – Remote `2:6`, Custom panels `2:7`, Prototype `2:8`, Handoff `2:9`
  - Cover phase labels: G1 state text `2:25`, G2 state text `2:30` (other phases are sibling rows of the same frame)
- **FigJam:** `bB3a7SnibAjHCvw6wuLPlO` (screen map section `15:944`, flows `15:945`–`15:954` and `11:799`)

**G1 kit** (page `2:3`): WF/Button `6:11`, WF/Field `6:24`, WF/Dropdown `6:25`, WF/Chip `6:34`, WF/Banner `6:50`, WF/Param row `8:72`, WF/Tree row `8:93`, WF/Nav rail `8:199`, WF/Status bar `9:90`, WF/Plot `9:91`. Frames: see the table in gui-design.md ("G1 wireframes"); Tune is `13:736`, Monitor `15:936`.

**G2 variables:** collections Primitives `VariableCollectionId:24:2`, Color `VariableCollectionId:24:64` (modes Dark `24:1`, Light `24:2`), Size `VariableCollectionId:24:112`, Type `VariableCollectionId:24:138`. Look variables up by name, not id.

**G2 sections** (page `2:4`): Shell `63:1180` (G3), About G2, Foundations `39:304`, Preview `40:304` (panels Dark `40:306`, Light `40:531`), QML mapping `40:753` (table `40:756`), Icons `26:2`, Buttons `29:2`, Inputs `31:18`, Status `33:69`, Param row `35:78`, Navigation `36:198`, Table `37:232`, Feedback `37:312`, Plot `38:274`, Tuning controls `42:690`.

**G2 component sets:** Button `29:104`, IconButton `29:139`, ArmButton `29:188`, TextField `31:37`, SpinBox `31:108`, ComboBox `31:131`, MenuItem `31:154`, Menu `31:174` (component), Switch `32:84`, CheckBox `32:119`, RefPicker `32:169`, StateChip `33:106`, Badge `33:189`, DeviceChip `33:208`, ParamRow `35:267`, NavItem `36:216`, NavRail `36:247` (component), TabButton `36:260`, TreeItem `36:347`, TableHeaderCell `37:240`, TableCell `37:268`, Banner `37:359`, Dialog `37:390`, Toast `37:414`, Tooltip `37:419` (component), ProgressBar `37:435`, SignalChip `38:288`, PlotFrame `38:417`, Slider `42:776`, GainControl `43:781`, CommandControl `43:885`, StatusBar `63:1182` (G3), CheckItem `63:834` (G3). Icons are components named `Icon/<lucide name>`.

**G3 frames** (page `2:5`, Screens – Local): Connect · Serial port `64:942`, Loading `65:1018`, Protocol mismatch `65:1225`; Device · Disarmed `67:1154` (the base the others were cloned from), Armed `69:1328`, ARM refused `69:1729`, Fault with drawer `70:1694`, Safe mode `70:2136`, Rename `73:2066`, Factory reset `73:2240`, Firmware update writing `73:2407`, waiting to confirm `73:2666`, rolled back `73:2949`; Overlay · Link lost `74:2992`, Device rebooted `74:3268`; checks: 1920×1080 Connect `75:2574` and Device `75:2721`, Light Connect `75:3039` and Fault `75:3186`. Header `75:3587`. Rows at y 0, 1640, 3280, 4920, 6560, 8200; frames 2760 apart.

**G4 frames** (page `2:5`): header `89:7038`, notes `89:7039`, row labels `89:7034`–`89:7037`. Row y 10040: Instance detail `84:3890`, Wiring picker open `85:4111`, Custom panel slot `85:4679`, Armed, layout locked `85:5034`. Row y 11680: Pending changes, refused op `87:4704`, Review pending changes `87:5149`, Discard all confirm `87:5221`. Row y 13320: Remove with dependents `89:5301`, After remove, wiring invalid `89:5729`, Failed instance `89:6270`, First boot, Add instance `89:6715`. Checks y 14960: 1920×1080 Instance detail `89:7040`, Light Pending changes `89:7326`.

**G5 frames** (page `2:5`): header `111:8758`, notes `111:8759`, row labels `111:8760`–`111:8763`. Row y 16840: Tune · Run results `99:6359`, Run in progress `103:6756`, Run stopped by disarm `104:7169`, Disarmed, new session `105:7584`. Row y 18480: A/B compare `106:7916`, Plant identification `107:8318`. Row y 20120: Monitor · Live plots and manual command `109:8621`, Paused with cursors `109:9147`, Link budget full `109:9731`. Checks y 21760: 1920×1080 Tune · Run results `111:8764`, Light Monitor · Live plots `116:9143`.

**G2 changes in G5:** Slider `42:776` variant `Scale=Log, State=Drag start` (`97:6372`); Tuning controls section `42:690` widened to 1880; mapping row `44:807` (Slider) extended.

**G6 frames** (page `2:6`, Screens – Remote): header `137:1561`, note `137:1562`. Each row is four frames, 2760 px apart. Row y 0: Agent `137:978`, Unreachable `137:1115`, Version mismatch `137:1255`, No device `137:1406`. Row y 1640: Device/ROS `138:10168`, Tune/ROS `140:1715`, Confirm live edit `140:2397`, Take from ROS `138:11284`. Row y 3280: Requesting `138:11399`, Lease granted/disarmed `140:2506`, Release `140:3097`, Revoked `140:3201`. Row y 4920: Agent history `138:10938` (drawer `141:13084`), Lease available `138:10582`, Monitor observer `141:12188`, Other GUI `141:12737`. Checks y 6560: Tune/ROS 1920 Light `143:3437`, Monitor observer 1920 Light `143:4234`.

**G2 changes in G6:** LeaseChip set `133:984` (Available `133:958`, Mine `133:963`, ROS `133:968`, Other `133:973`, Requesting `133:978`, Revoked `133:983`), mapping `133:1036`, instance preview `133:1045` in Shell `63:1180`. StatusBar `63:1182` optional Lease boolean. Dialog `37:390` optional Confirmation checkbox (mapping `40:937`). PlotFrame `38:417` variant State=Remote live `140:978` (mapping `40:982`). No new variables.

**G7 frames** (page `2:7`, Custom panels): header `162:2714`, note `162:2715`. Three columns 2760 px apart. Row y 0: Live calibration `161:1030`, Fully calibrated `161:1344`, Read snapshot/export `161:1647`. Row y 1640: Imported file review `161:1965`, Applying `161:2344`, Applied `161:2669`. Row y 3280: File rejected `161:2988`, Apply failed `161:3291`, IMU data unavailable `161:3615`. Row y 4920: Confirm apply `162:2565`, Waiting for first sample `165:3124`. Checks y 6560: Live calibration `162:2887`, Confirm apply `163:8006` (x 2120), both 1920×1080 Light.

**G2 changes in G7:** Sensor panels section `160:992` at x 5544, y 4200. CalibrationLevel set `160:999`, variants 0 `160:994`, 1 `160:995`, 2 `160:996`, 3 `160:997`, Unavailable `160:998`; Label text property. OrientationPreview set `160:1050`, Live `160:1047`, Stale `160:1048`, Unavailable `160:1049`. Mapping rows `160:1132`, `160:1141`. Instance reviews Dark `160:1087`, Light `164:1030`. Existing variables/styles only.

**G2 changes in G4:** Banner `37:359` booleans `Button 2`, `Button 3`; MenuItem `31:154` boolean `Show detail` and a Detail text; mapping rows `40:928` (Banner) and `40:820` (Menu / MenuItem) extended.

**Cover:** G0 `2:20`, G1 `2:25`, G2 `2:30`, G3 `2:35`, G4 `2:40`, G5 `2:45`, G6 `2:50`, G7 `2:55`, G8 `2:60` (state texts).

## Figma working notes (traps found so far)

- Call `create_session` with the file key before any other Figma tool, and pass its id on every call.
- `use_figma` calls must run one at a time. Switch page once per call.
- The helper prelude for G2-style work is in `.claude/skills/gui-design-step/figma-prelude.js`. Paste it at the top of each `use_figma` call that builds with the design system.
- Binding a TEXT component property makes every variant show the property's default, and editing a bound text in one variant rewrites that default for all. For text that differs per variant, don't use a property; set the characters per variant.
- A cloned variant loses its `componentPropertyReferences`; rebind them.
- `figma.createAutoLayout()` makes an empty frame 100×100. A spacer must be resized to 1 px on the cross axis, or it makes its row 100 px tall.
- `resize()` sets both sizing modes to FIXED. Set `primaryAxisSizingMode` / `counterAxisSizingMode` back to `AUTO` after it when the frame should hug.
- `figma.flatten()` of SVG children into a component that sits in a section keeps page coordinates; fix the glyph position afterwards.
- Apostrophes in `description` get HTML-escaped. Write descriptions without them.
- A section's blocks are stacked with the `block()` helper; if something was created in the section before `block()` runs, use `restack()` to remove the gap.
- Text that fills a fixed-width parent needs truncation (`textAutoResize = "TRUNCATE"`, `maxLines = 1`) or it wraps and grows the row.
- Check visually with `return await node.screenshot({scale: …})` inside `use_figma` after each section.
- The Plugin API cannot resize a child of an instance (`resize()` is ignored silently). ProgressBar takes its value through the Track's `paddingRight`; recompute it after the bar's width changes (for example in a 1920 copy).
- KV rows are named by their key, so a key can clash with a card name ("Configuration", "Faults"). Find cards by name whose parent is a "Column N" frame.
- On a failed `use_figma` call, follow `safeToRetryWithoutCanvasRead` when supplied. Otherwise inspect the canvas before retrying; a transport error may follow a successful mutation.
- `findOne(TEXT)` inside a row that holds an instance can return the instance's own text: pick the row's direct TEXT child (`row.children.find(...)`). `instance.resetOverrides()` undoes such a mistake.
- The ProgressBar Indeterminate variant has a single label text.
- Absolutely positioned overlays (drawer, dialogs, tooltips, toast) keep their 2560 coordinates in a resized copy; move them for 1920 checks.
- A scrim made from `color/bg/app` does not dim in Light; use `color/bg/scrim`.
- `resize()` on a TEXT makes its width FIXED too. To keep a truncated one-line text filling its row, set `layoutSizingHorizontal = "FILL"` after the `resize()`.
- A nested instance inside an instance can take `layoutSizingHorizontal = "FILL"` (MenuItems in a widened Menu instance), unlike `resize()` on instance children.
- ParamRow Normal's Note text clips to "…" when given characters: leave it " " and put the text elsewhere.
- The Switch swapped into a ParamRow keeps its default label "Show legend": set the Label to " " after `swapComponent`.
- A temporary resized copy with absolute overlays needs them moved: the scrim to 1920×1024 at y 56, dialogs recentred, popovers and tooltips moved.
- Swapping a StatusBar instance's State keeps the text overrides, but a nested Faults badge takes the new variant's tone: set the tone again after the swap.
- `color/text/tertiary` is 3.8:1 on surface in Light: use `color/text/secondary` for small caps headers.
- Building with a subagent works: give it the prelude path, the session id, the component ids and the traps, and review its frames before recording.
- The position of a child inside an instance can't be overridden, and a child fill can't be resized: only fills, visibility and characters. Bake a variant for a position you need (Slider Drag start).
- `vectorPaths` on a new vector places the path at its own coordinates; on an existing vector it keeps the old origin and shifts the path, so set `x`/`y` to the path's min point afterwards.
- Paint `opacity` on a variable-bound paint is dropped when rebuilt as `{...p, opacity}`; fade traces with the node's `opacity`.
- `effectStyleId = undefined` throws; leave it unset.
- A second `node.screenshot()` in the same call can return the first image again: one screenshot per call.
- An instance resized wider (GainControl) does not stretch its Slider or Buttons; nested `layoutSizingHorizontal = "FILL"` widens only the frame.
- `swapComponent` on a nested instance keeps its text overrides. A SignalChip hugs its name when Value and Unit are hidden.
- PlotFrame Live is a fixed-coordinate component: its Plot area cannot take FILL while the main component has layout NONE. Use G6's Remote live variant, whose root is vertical auto-layout.
- Reusing the G5 Tune gains and swapping GainControl Dragging to Default resets nested Slider tick-label overrides: restore the range labels to match the number field (for example 0.21 / 0.85 / 3.40).
- Monitor's copied plot areas are initially fixed-size. At smaller widths, set plot areas to fill both axes, wrap legends, scale vector geometry and explicitly refit axis text. SCALE constraints can narrow text enough to wrap; restore WIDTH_AND_HEIGHT and place labels within bounds. Parse negative values from the node name or normalise the Unicode minus.
- A Figma transport error can occur after a mutation was applied. Read the canvas and find exact returned/named check frames before retrying; do not assume every failed call rolled back.
- Growing the QML mapping table can make its section overlap Icons. G6 moved the mapping section below the other sections in its column; check section bounds after adding mapping rows.
- Returning every descendant id for many cloned screens can exceed the 48 KB use_figma return limit and roll back the call. G7 split check creation into bounded groups after inspecting the canvas; do not silently omit affected ids.
- Narrowing a generic table card does not shrink fixed-width TableCell instances. Resize the columns explicitly or values can disappear beyond the card. Likewise, shorten or wrap the signal-card header action when its title changes to paused/stale.

## Session log

| Date | Session | Work |
|---|---|---|
| 2026-11-14 | G7b planning | Recorded six user answers: leave G7 in review, one Add menu, detailed loop headers, delete obsolete invalid-layout frame, second real-time loop for wiring refusal, shipped fusion.mock. Read current core v8/loop/source contracts; inspected G7 and G4 structure/screenshots. Revised Parts A–G to 17 new Layout frames, two G4 deletions and shared-screen updates with size/theme checks. No Figma mutations. Awaiting scope approval before Part A |
| 2026-11-13 | loop design (firmware session) | After firmware F3d: compared the designs with the built loops and the planned sources; user decisions (loops edited on Layout, Remove follows the firmware, sources designed as provisional, third sample loop `fusion`); wrote the G7b brief in gui-design.md, updated the sample device and the state inventory, closed todo-layout-ops.md. Nothing built in Figma |
| 2026-11-13 | gui design 5 | G6 accepted, including LeaseChip, StatusBar Lease, Dialog Confirmation and PlotFrame Remote live. User requested a PR into firmware_overhaul, followed by G7 in this session |
| 2026-11-13 | gui design 5 | Opened PR #39 into firmware_overhaul; started G7 orientation and discussion. Custom panels page is empty; G4 Calibration slot inspected. Layout, file workflow and orientation preview await user decisions |
| 2026-11-13 | gui design 5 | G7 choices recorded: dashboard, local file review before explicit Apply, numeric values plus graphic. Added G2 CalibrationLevel and OrientationPreview, 11 Calibration screens, all 1080p Light checks and 2 retained copies. Fixed generic signal columns, paused values, retry wording and compact file controls. G7 awaiting acceptance on the design branch. Discovered #39 had merged through 5bf0612 and been reverted by #40; corrected PR metadata to its merged scope and preserved the revert |
| 2026-11-13 | gui design 4 | G5 accepted, including Auto re-run default; created Codex/gui-design from current firmware_overhaul and carried prior design records forward; G6 decisions, four G2 extensions, 16 remote frames and two retained 1080p Light checks. Self-review fixed ARM without a lease, gain-range labels, no-device connection state and responsive history/plots. G6 awaiting acceptance |
| 2026-11-11 | gui design 1 | Phases planned; G0 (FigJam flows, state inventory); G1 wireframes |
| 2026-11-12 | gui design 1 | G1 decisions (any client may disarm; device-name to-do); G2 design system; tuning controls (sliders, steps, gain history, auto re-run, manual command Latch) in G2 and the G1 wireframes; this status file and the `gui-design-step` skill |
| 2026-11-12 | gui design 2 | G2 accepted; G3 decisions (status bar zones, inline Save/Revert, loops and core bars, checks kept, Connect card, Rename dialog, OTA frames, link-lost modal); G2 additions (StatusBar, CheckItem, Disarm disabled, scrim, Unsaved contrast); 15 G3 frames and 4 check copies |
| 2026-11-12 | gui design 3 | G3 accepted; G4 decisions (frames, remove with dependents, wiring picker, detail pane, loop at add, pending review, add flow); firmware to-do todo-layout-ops.md; G2 additions (Banner buttons 2 and 3, MenuItem detail); 11 G4 frames and 2 check copies, built by a Sonnet subagent and reviewed (Failed instance status bar and a banner text fixed) |
| 2026-11-12 | gui design 3 | G4 accepted; G5 decisions (frames, gain of 0, negative gains, Auto re-run after a first Run, Run disabled while disarmed, run colours, stacked plots, link budget refusal, no Save on Tune); G2 Slider Drag start; 9 G5 frames and 2 check copies, built by a Sonnet subagent and reviewed (Paused live values and the plant-identification warning colour fixed) |
