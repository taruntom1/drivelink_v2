# H2d Monitor: plan and results

Brief: [host-steps.md](host-steps.md) "H2d Monitor". Design: G5 Monitor frames
`109:8621` (live plots and manual command), `109:9147` (paused with cursors),
`109:9731` (link budget full), Light check `116:9143`; G6 1080p fitting
reference (Monitor observer `141:12188`). File `49NRHfKe8rOoVlloLLCqLh`.

## User decisions (2026-11-17)

| Topic | Decision |
|---|---|
| Split | **H2d-1** plots (this PR) and **H2d-2** manual command, one session and PR each. Both get an independent Opus review at high effort: H2d-1 for the shared subscription slots and the render-thread data path, H2d-2 for the command path. |
| Hold rule (H2d-2) | Only numeric inputs whose descriptor range is finite and includes 0 can be commanded. At connect each loop's inputs split into a **value group** (those inputs, hold 0, the value the firmware sets at ARM) and a **keepalive-only group** (inputs without such a range, e.g. `position_sp`) that never carries values. Every value sent is one shown on a slider or 0. Non-commandable inputs are listed disabled with the reason. Before this GUI writes a live param to an instance it is commanding, it discards queued values. |
| Subscription slots | The Layout live group is released while Layout is not shown and set up again on return; loop statistics stay. Monitor uses free slots only, with the intent-first, exact-match rules, and refuses an add when no slot is free. |
| Hardware | vdev for everything, plus a short bare-board run on COM3 (1 kHz plotting near and below 100 % budget; predicted vs measured rates and lost frames from device counters; nothing flashed). |

## H2d-1 scope

Plots only. Manual command is H2d-2; its card shows a short "arrives in H2d-2"
placeholder in this PR.

**Backend (pure, `backend/monitor_model.*`).**
- Signal catalogue for the browser: per loop (its statistic signals) and per
  running instance (signals and inputs, "(input)"), with unit and value type;
  from descriptors only.
- Plot rates per loop: the loop rate divided by 1, 2, 5, 10, 20, 50, 100, …
  down to about 1 Hz; default about 100 Hz (or the loop rate if lower).
- Link budget (ESP → host at 921 600 baud, 92 160 B/s): each subscription
  group `9 + 12 + 4k` bytes at its rate, including the GUI's loop-statistics
  and Layout live groups and the status stream. `checkAdd` refuses over 100 %
  or with no free slot / over `limits.group_entries`, with the reason and the
  highest plot rate for that loop that fits ("Plot control at 500 Hz (53 %)").
  Nothing changes by itself.
- Group plans: one subscription group per plotted loop, its refs in plot order,
  divider from the chosen rate.
- Plot data: per-signal ring of (device µs, value) sized for the longest
  window (60 s) at the plot rate; shared time axis on device time (all loops
  share the device clock); "now" is the newest sample, frozen while paused.
- Y axis fitting with round ticks; min/max decimation per pixel column; cursor
  readout (value at A and B, Δ, Δt); lost frames from counter gaps plus ring
  drops; Snapshot CSV text.

**Connection (`backend/connection_monitor.cpp`).** `syncMonitor` mirrors
`syncLive` for several groups: free slots only, after command setup, loop
statistics and the Layout live group; intent recorded before SUB_CONFIG; reuse
or clear only on an exact LIST match; one mutation at a time, the latest plot
set wins; Busy/Withdrawn retried twice; a SUB_CONFIG landing on a newer boot
releases its slot. `pollMonitor` pops every sample from each group's
`GroupBuffer` into the rings on the GUI timer. Cleared on close, Boot reloads
the plan. The Layout live group follows the page (`setPage`).

**Plot item (`PlotItem`, C++ `QQuickItem`).** Scene-graph line strips, one
geometry node per series, from the decimated ring; flat-colour material; no
QtGraphs. Axes, grid, labels and cursors in QML around it.

**QML.** `MonitorPage.qml` (G5 three columns): Signals card (search, browser
by loop/instance with live values and checkboxes, plot rates per loop, link
budget bar, groups n of 8, lost frames; a refused add shows the reason and the
one-click lower rate), toolbar (Pause/Resume and Space, Window 2/5/10/30/60 s,
Cursors, Snapshot CSV, Add plot), stacked plots with one shared time axis (one
per unit; a checked signal joins the plot of its unit, else the first empty
plot, else a new one; legend chips with live value and remove), A–B cursors
across every plot with a readout table while paused, and the manual command
placeholder card. 1080p: plot areas fill and share height, legends wrap,
the paused toolbar wraps. Layout's Signals card gains **Plot in Monitor**.
Snapshot CSV writes `Documents/DriveLink/snapshots/<device id>-<time>.csv` and
shows the path in a toast: no file dialog (the OTA picker stays the only native
dialog).

## Tests

- `monitorLogic` (pure): catalogue, rate options, budget against the
  architecture table, refusals and the suggested rate, group plans, rings and
  windows, decimation, ticks, cursor readout, loss counting, CSV.
- `monitorOperations` (vdev): groups on free slots, add/remove/rate change
  reconfigure, samples reach the rings, no free slot refused, Layout live group
  released off-page and restored, Boot, close clears exactly what it owns.
- `monitorInShell` and a Qt Quick test of the plot frame: page in the shell,
  Plot in Monitor, pause, cursors, Space not taken from a text field.
- Plot item performance: 16 series at 1 kHz over a 10 s window offscreen,
  render-time bound logged and loosely asserted.
- Screenshot scenarios `monitor`, `monitor-paused`, `monitor-budget` (both
  sizes, Dark and Light).

## Hardware check

vdev: backend tests and the screenshot campaign. Bare board (COM3, existing
firmware, nothing flashed): `benchMonitor` (env-gated like `benchBoard`) plots
the control loop's statistics at 1 kHz and a set near 100 %, then compares the
predicted rate with the measured one from device counters and timestamps and
expects zero lost frames under 100 %; it restores the board.

## Deviations to log

Snapshot CSV without a dialog; plots and rates are not persisted (per
connection); the time axis is device time; Layout's live group follows the page
(user decision); the hold rule for H2d-2 (user decision; architecture and
brief updated).

## Docs

`host/gui/README.md` (Monitor), `host/CLAUDE.md` (Monitor subscriptions,
Layout group off-page, the H2d-2 hold rule), `plans/host-architecture.md` (hold
rule), `plans/host-steps.md` (split rows), `plans/host-status.md`,
`plans/gui-design.md` (closed issues), `plans/hardware-log.md`.

## Results

Built as planned on `claude/overhaul-H2d`. Files: `backend/monitor_model.*`
(pure), `backend/monitor_view.*` (`MonitorView`, QML `connection.monitor`),
`backend/connection_monitor.cpp` (`setPage`, `syncMonitor`, `pollMonitor` on
a 50 ms timer); `OwnedSub` gained `monitor`, `divider`, `statistics()`;
`connection_live.cpp` keeps the live group only while `m_page=="layout"`;
`app/plot_item.*` (`PlotItem`, `QML_ELEMENT` in `DriveLink.Shell`),
`app/MonitorPage.qml`, Shell/LayoutPage wiring, `main.cpp` `--plot`,
`--rate`, `--window` and two screens; `tests/test_monitor.cpp`
(`gui_monitor_tests`), `tests/test_support.hpp`; three screenshot scenarios.

**Changed from the plan.**
- A subscription group carries at most 1 kHz: the firmware link task
  (`core_link.cpp`, 1000 µs period) sends each group's latest sample once
  per run. Found by the first bench run (a 2 kHz loop at divider 1 gave
  1000 frames/s, two ticks apart, the rest counted lost). The GUI caps a
  group's rate (`kMaxGroupRateHz`, `minDivider`): choices start there, a
  chosen divider rises with a live loop-rate change, and the budget counts
  at most 1 kHz per group. `monitor-budget` now runs the sensor loop at
  1 kHz live (twelve controller signals 76 %, the IMU's first signal 102 %
  refused, "Plot sensor at 500 Hz (89 %)").
- A lost link keeps the plot set and rates (`MonitorView::lose()`, same
  device only), as Layout keeps its selection: a requested reboot over TCP
  reopens the link. Found by the vdev Boot test added for the review.
- `benchMonitor` plots near 100 % with a second, non-owned 1 kHz group, not
  with a 2 kHz loop, and restores the board on every exit.

**Internal choices** (decision log): the 1 kHz cap; software renderer path
(a cosmetic 1 px image; 16 series × 10 k samples ≈ 17–21 ms a frame,
decimation ≈ 7 ms; line strips on RHI, checked once with D3D); lost frames
are counter gaps; live values for plotted signals only; at most 16 signals,
8 plots, windows 2–60 s; Resume clears the rings, cursors only while paused;
loop rate from the live `rate_hz` param; Monitor groups persist across pages
and over a lost link; a failed pass retries three times; Snapshot CSV without
a dialog; plots and rates not persisted.

**Review.** Self-review: latest values only for the newest sample's count;
RHI line width 1. Independent Opus review (high): no path touches another
client's slot. Fixed: a removed signal's value revived by an in-flight pass;
more than 8 plots via distinct units; a load-lowering rate change refused
while over 100 %; a stale unanswered intent of one record kind could match
another kind's group on the same slot (a successful configure now drops
every other record for that id; statistics lookups filter by kind); lost
frames emitted per gap; no retry after a failed pass; entry limit unknown
before the first LIST; the rate combo after a live rate change; Space under
shell overlays; tests added. Follow-up review: fixes correct; my first Space
fix broke Space after a click (reverted to text fields only); Disconnect
after a lost link now resets Monitor. Not changed (reviewer agreed): no
per-group "samples stopped" indicator; `others` unknown until Monitor's
first LIST.

**Verified.** `validate.py full` passed; gui-backend and gui-monitor on vdev;
screenshots `monitor`, `monitor-paused`, `monitor-budget` (both sizes, Dark
and Light; software, and once on RHI); bare board COM3 (`benchMonitor`):
41 % → 1000.00 Hz, 1 tick/sample, 0 lost; 81 % beside another 1 kHz group →
1000.00 Hz, 0 lost; loop at 2 kHz live, 82 % → 1000.00 Hz at divider 2,
2.000 ticks/sample, 0 lost; board restored (rates 1000/100/10 Hz, idle
groups, flags 0, bench op refused). See [hardware-log.md](hardware-log.md).
