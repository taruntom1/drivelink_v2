# H2c-2 plan and results: Layout live data and actions

Approved by the user on 2026-11-17 (Opus 5.5, high effort; independent review
required by the brief). Branch `claude/overhaul-H2c-2` from
`origin/host_overhaul`; PR into `host_overhaul`. H2c-1 (#78) is merged.

## User decisions (2026-11-17)

- **Actions while armed follow the device.** Loop actions stay enabled while
  armed; paused actions on an instance in a real-time loop are disabled while
  armed with the reason.
- **Blobs are hex only.** Blob in: a hex field (at most 512 bytes) checked
  locally. Blob out: size, hex and Copy. No new native dialog (the OTA image
  picker stays the only one); calibration files belong to H2f's panel.

## Scope

The live half of the Layout detail pane: Signals (live), the value/age/state
on Sources rows, loop Statistics, the Actions card, the custom panel slot.
No wire, firmware or library change. About 2–2.5k lines, one PR.

## Changes

- **Selected-node subscription** (`host/gui/backend/connection_live.cpp`, new):
  - One GUI-owned subscription group for the selected running node: an
    instance's signals plus its sources' `<name>.value` / `<name>.age_us`; a
    loop's `ticks`, `overruns`, `last_run_us`, `max_run_us`. Divider
    `rate/10` (~10 Hz). More signals than `limits.group_entries`: the first N,
    and the card says how many are hidden.
  - Free slots only, after command setup and the Device loop-statistics
    groups; cleared only on an exact owned match (same rule as loop stats).
  - Reselect: reconfigure our own slot only after LIST shows it still exactly
    ours, else take a free slot. One mutation in flight; latest selection wins.
  - Clear when the node is not running (pending, failed, offline, safe mode)
    and on close. Busy/Withdrawn retried twice. "No free subscription slot" or
    the refusal shows in the card. No new sample for ~1 s: Telemetry
    unavailable.
- **Pure rules** (`host/gui/backend/live_model.*`, new, tested without a
  device): signal rows (generated source signals excluded); loop statistics and
  budget (last run over period); source state: not connected, pending (a new
  target is never shown live), waiting, valid, stale (age above 3 producer
  periods, "No recent sample"), missing ("X is not running"), telemetry
  unavailable; age sentinel `0xFFFFFFFF` hides the value; action argument
  parsing (`parseValue(ScalarDescriptor)`), typed results, hex blobs; action
  block reasons.
- **Actions:** `runAction(itemKey, args, blobHex)` sent once on the service
  executor, one at a time; a timeout shows "may have run; not retried";
  `refreshConfigAsync` after each (paused actions can stage params and set
  Unsaved). Blocked: instance not running, pending add/remove, paused on a
  real-time loop while armed, OTA/reboot (`commonBlock`), another action in
  flight.
- **QML:** `LayoutPage` gains Signals (live), Statistics (loops) and Actions
  (name, "runs in loop" / "paused action", Run; arguments or a blob open an
  inline form in the card, not a modal; results under the row). `TargetRow`
  shows value, age and state (G7b frames). Custom panel slot: a second tab when
  `DriveLink/Modules/<type name>/Panel.qml` exists in the resources, loaded
  with `backend` and `nodeKey`; none ships until H2f; a test-only panel checks
  the slot.

## Tests

- gui-backend pure (`layoutLogic`): every source state, stale threshold and
  sentinel, argument/hex parsing, action block rules, statistics/budget.
- gui-backend against vdev: selection subscription values; reselect keeps our
  slot; foreign groups untouched; no-free-slot note; cleared on close and when
  the node stops running; sources valid / not connected / missing / pending,
  stale by `benchStall` of the producer loop; `feedback.mock sample` results;
  `imu.mock calibration_read` (blob out) → `calibration_write` (blob in);
  `actuator.mock reset` (paused, real-time loop) locally blocked while armed;
  an action timeout through vdev slow replies is uncertain and sent once.
- `tst_layout.qml`: action form (argument validation, hex rejection), signal
  and source row states, the custom panel tab with a test panel.

## Hardware check

vdev only (brief). Scenario campaign adds `layout-signals`, `layout-loop`,
`layout-sources` (valid, then stale/missing) and `layout-action`, and
re-renders the existing Layout scenarios. No bare-board run: no firmware or
transport change; the free-slot subscription path was checked on COM3 in
H2b-1.

## Review

Independent Opus review at high effort: subscription ownership, rebind and
clear; send-once actions and uncertain outcomes; the armed action rule; GUI
thread delivery.

## Deviations (decision log)

- Stale threshold is 3 producer periods only: no descriptor marks a consumer's
  own limit param.
- Signals card lists signals only (no inputs, no generated source signals).
- "Plot in Monitor" waits for H2d (note for H2d).
- The two user decisions above.

## Docs

`host/gui/README.md` (Layout), `host/CLAUDE.md` (subscription and action
rules), `plans/gui-design.md` (open issues), the status file and handoff,
`plans/hardware-log.md` (vdev campaign row).

## Working record (for the build after a context compaction)

- **Existing patterns to copy.**
  - Loop-statistics subscriptions: `LocalConnection::setupLoopStats` and
    `pollLoopStats` in `host/gui/backend/connection.cpp` (~line 294–430):
    free-slot LIST, intent recorded in `c->subs` before SUB_CONFIG,
    Busy/Withdrawn retry, `session->subscribeBuffer(id, epoch, width, 8,
    generation)`, `buffer->latest(sample)`. Gating in `refresh()` (~line 265):
    `commandsSettled`, `m_subsPending`, `m_subsGeneration`.
  - Ownership: `OwnedSub`, `matchesSub`, `Context::subs` and exact-match clear
    in `host/gui/backend/connection_internal.hpp` (lines ~39–101).
  - Timer: 100 ms (`m_timer`), `pollLoopStats` every third tick.
  - Device ops off the GUI thread: `session->async<T>(work, completion)` with
    `QPointer self` and `current(c)` checks; `runOnDevice` for tests.
  - Layout data: `LocalConnection::layout()` in `connection_layout.cpp`
    (~line 80) builds `node`, `wiring`, `sources`, `members`; selection in
    `selectNode` / `rebuildLayout`; `refreshConfig()` after mutations.
  - QML: `host/gui/app/LayoutPage.qml` (right `sideColumn` holds Wiring and
    Instances in this loop; `Card.qml`), `TargetRow.qml` (wiring/source row),
    `DLReasonHover` for disabled reasons, `safetyKeys` Esc handling.
- **Library.** `Client::action(Ref, std::array<uint32_t,4>, span<uint8_t>)`
  returns `ActionResult{values, blob}`; blob > 512 is a Usage error. Descriptor
  fields: `ItemDescriptor::runs_in` (`ActionPlacement`), `blob_direction`
  (`BlobDir`), `args`/`results` (`ScalarDescriptor{value_type,name,unit}`).
  `parseValue(ScalarDescriptor, text)` and `decodeValue(type, bits)` in
  `drivelink/types.hpp`. Default request timeout 1500 ms.
- **Protocol facts.** Subscriptions may change while armed; refs must be bound
  signals/inputs of running instances on the named loop; a loop instance's
  statistics use the loop's own id as the group loop. Paused actions on
  instances in any real-time loop are refused while armed.
- **vdev mocks for tests.** `actuator.mock` (real-time required): `drive`
  (loop, duty f32 → velocity, position), `reset` (paused, blob both),
  `diagnostic` (loop). `feedback.mock`: `sample` (loop → velocity, position).
  `imu.mock` (non-real-time): `sample`, `reset`, `calibration_read` (blob out),
  `calibration_write` (blob in), `calibration_apply`, `set_mode(mode u8)`, all
  paused except `sample`. `fusion.mock`: sources `feedback`, `imu`; signals
  `feedback.value`, `feedback.age_us`, `imu.value`, `imu.age_us`, `combined`.
- **Figma.** File `49NRHfKe8rOoVlloLLCqLh`: Instance detail `84:3890` (Signals
  table Signal/Value/Unit; Actions rows with Run), Loop detail `182:10790`
  (Statistics · live: Ticks, Overruns, Last run, Longest run, Budget "4.1% of
  1000 µs"), Sources stale/missing `183:15772` (value and age right of the
  picker; state line under it: "Missing · left_enc is not running", "Stale ·
  No recent sample · threshold 30 ms (3 × 100 Hz periods)"; footnote "Invalid
  sampled values are hidden…"), Custom panel slot `85:4679`.
- **Validation.** `python tools/validate.py` from the repo root; iterate with
  `--only <check>`; `full` once in the background before review; read
  `build/validate/summary.json` (`finished_at`, `source`,
  `full_gate_passed`).

## Build progress (working record, second compaction point)

- Built and committed: 41270fc (build), then the self-review fixes (outcomes
  cleared on Boot, pollLive snapshot guard). `validate.py full` passed on
  41270fc (all 7 checks); the self-review commit still needs the full gate
  after the review fixes.
- Code: `connection_live.cpp` (syncLive/pollLive/live/checkAction/runAction/
  updateActions), `live_model.*` (pure rules + `ActionTable`), `ActionRow.qml`,
  LayoutPage (Statistics, Signals · live, Actions cards in a scrolling side
  column; panel tab via Loader), TargetRow (value/age/state), app `--action`.
- vdev controls added: `drop-action-reply N`, `loop-stall LOOP MS`, stats
  `actions N`. Scenarios added: layout-signals (114:0), layout-action
  (113:0, runs 113:0:768), layout-sources-stale (loop-stall 0 8000);
  layout-loop now shows Statistics. Rendered OK (scratchpad shots).
- Tests: `liveLogic`, `liveDataAndActions`, `layoutLiveInShell` pass; the
  gui-backend test target needs `-Wa,-mbig-obj` on MinGW (too many sections).
- Found while building: a Repeater over a plain list recreated ActionRows on
  any row's data change (lost form text; the test read a freed row and
  crashed in a TLS destructor). Fixed with ActionTable (in-place
  dataChanged); the arg fields use a count model. The H2c-1 Wiring/Sources
  rows survive status frames (a probe showed no recreation without a data
  change).
- Deviations to log: QML form tests live in the backend shell test (the
  quick-test runner loads only DriveLink.Style); "missing" has pure tests
  only, no screenshot; a failed live setup (no free slot, Busy) retries on
  reselect only; a paused action that sets Unsaved leaves no line in this
  window's change list (the notice may say "made outside this window"); the
  test-only vdev controls and app `--action`.
- Next: independent Opus review (brief in the scratchpad: review_brief.md;
  rewrite it from this record if lost), fix, full gate, final scenario run,
  hardware-log row, docs, status, PR.

## Results

Filled in after the build, review and vdev campaign.
