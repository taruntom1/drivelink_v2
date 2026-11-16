# H2a-2 GUI shell and backend

Status: approved by the user 2026-11-15; built, reviewed and checked on vdev
2026-11-16. Results below; PR into `host_overhaul`.
Branch: `codex/overhaul-H2a-2`, from `origin/host_overhaul` at `501837a`.
PR target: `host_overhaul`. H2a-1 is merged in PR #73.

## Decisions settled before the plan

- The user approved Codex planning/building with an independent Codex review
  at high effort, replacing the unavailable Opus models for this step.
- On connection, create GUI command groups and send prefix-only keepalives;
  do not send descriptor defaults or infer semantic hold values. Explicit
  input commands belong to the later screen steps.
- If already armed, observe with DISARM available. Do not adopt another
  owner's groups, extend their watchdog receipts or change command bindings.
  Create GUI groups after an explicit disarm has been observed.

## Scope and size

Build the application and local connection backend, the shell, arm/disarm/ack,
command-group lifecycle, generic item-model foundations, vdev scenario hooks
and the shell screenshot tool. Reuse H1b's async services and managed I/O
keepalives and H2a-1's style controls.

Keep this step together so one independent review covers the interaction
between Qt delivery, Esc/DISARM, arming and command-group ownership. Estimated
size: about 3–4.5k handwritten lines including tests and documentation. If
implementation reveals a materially larger scope, propose a split for the
user's approval before expanding the step.

The Connect page has only an endpoint field, connect/disconnect and loading
or error feedback. Device, Layout, Tune and Monitor have shell placeholders.
Full screen content, serial discovery/recents, rename, SAVE/REVERT workflows,
OTA, fault/history presentation and badge policy, reboot notices and link-loss
recovery UX stay in H2b onward. Agent mode stays in H3b.

## Changes

- `host/gui/`: add the `drivelink-gui` executable, a reusable backend target
  and the shell QML module, linked to the existing style and libdrivelink.
- Local connection behind a small connection interface. Opening the session
  and initial metadata loading run off the GUI thread. Session completions
  and model events enter Qt through a FIFO queued dispatcher. QObject changes
  and model notifications run only on the GUI thread. Bound pending GUI work;
  guard callbacks with connection generations and retain the dispatcher until
  accepted callbacks have drained during teardown.
- Expose connection/loading/readiness, identity, arm state, last-disarm reason,
  dirty/pending/safe-mode flags and typed errors. Preserve refusal result/detail,
  request context and uncertain mutation outcomes; never retry a mutation
  automatically. ARM requires complete current metadata and ready GUI groups.
- Route every DISARM request directly to `Client::disarmAsync`, bypassing
  ordinary backend/service queues, including while discovery or ARM is pending.
  An Esc arriving during ARM also fences that attempt: its late completion
  cannot leave the device armed or restart GUI traffic.
- Add the nav rail (Device, Layout, Tune, Monitor), status bar bound to backend
  state, Ctrl+1..4 page selection, banner area, faults drawer frame and bounded
  toast stacking. Use Theme values and existing controls. The page scrim starts
  below the status bar and blocks the underlying page while DISARM stays live.
- Install an application event filter for Esc, including shortcut override
  handling. Esc requests DISARM, cancels the active text edit and dismisses
  a popup/dialog as appropriate. Focused fields and open popups cannot consume
  the safety action; cancelled edits must not commit on focus loss.
- Discover commandable inputs from running configured instances. Allocate only
  free command slots, partitioned by owning loop and advertised entry/payload
  limits. With no inputs, use an empty keepalive group on a running loop.
  Safe mode, no running loops, occupied inputs or insufficient capacity retain
  observation/DISARM and explain why ARM is unavailable. Never clear or steal
  pre-existing groups. Fail partial setup cleanly and reconcile uncertain
  configuration results before cleanup or retry.
- Keepalive handles send prefixes only. There is no GUI value-command API in
  this step. Explicit disconnect/close requests DISARM when this GUI owns
  groups, then stops keepalives and clears only its groups when confirmed
  disarmed. An observer disconnect does not disarm another owner. Lost links
  invalidate ownership/handles; Boot invalidates generations and reloads
  metadata. Rebuild groups only after a successful reload and confirmed
  disarmed state; never replay input values or ARM.
- Generic read-only Qt model foundations: instance/loop hierarchy and reusable
  descriptor rows for parameters, signals/inputs and actions. Keep stable keys,
  discovered names/types/units/ranges/enums and configured/pending distinctions.
  Editing, live subscriptions and action execution belong to their screen steps.
- `host/vdev/`: add explicit test-only startup/timed scenario controls for a
  fault, safe mode, link stall, reboot and slow replies. Use real native core
  paths where applicable, bounded scheduling and existing device frames;
  default operation is unchanged. No new production wire messages.
- Grow the gallery rendering approach into a shell screenshot mode/tool:
  named available shell states, readiness-based capture with a timeout,
  2560×1440 and 1920×1080, Dark and Light. Support an isolated vdev fixture or
  a supplied endpoint. Mutating scenario injection is vdev-only and explicit;
  a supplied physical endpoint is not provisioned or fault-injected.

## Tests and validation

- C++ tests for generic rows/tree structure, sparse IDs, pending/configured
  separation, command-group planning/capacity, refusal mapping and generation
  guards. Reuse the repository's existing test dependencies.
- Backend integration against vdev: async loading without freezing Qt; callback
  thread/lifetime and disconnect/reconnect races; keepalives without value
  writes; multiple loops and capacity refusal; preservation of existing groups;
  ARM/refusal, DISARM and acknowledgement; observer-to-owner transition;
  partial setup cleanup; Boot/link-loss invalidation and no automatic ARM.
- Exercise DISARM with an ordinary request delayed and while ARM is pending;
  assert final device state, not just the emitted GUI signal. Fault scenarios
  verify acknowledgement refusal while active and success after recovery.
- Qt Quick/application tests: Esc with a focused edited TextField and an open
  popup/dialog, cancellation without committing, DISARM above the scrim,
  navigation keys, state bindings, drawer frame and toast lifecycle.
- Screenshot smoke checks for both sizes/themes and available shell scenarios;
  inspect the rendered shell for clipping, status-bar placement and scrim bounds.
- Register tests with CTest in the existing `host-win` and `host-linux` checks.
  Iterate on focused tests; finish with `python tools/validate.py full`, zero
  warnings and no omitted checks. Verify current source/timestamps and
  `full_gate_passed` in `build/validate/summary.json`.

## Review and hardware check

Self-review plus one independent Codex review at high effort: required because
this step touches arming, emergency DISARM, command keepalives and Qt threading.
The reviewer receives this plan, scoped rules, diff base, validation results
and the ownership/teardown/ARM-race risks. Verify findings, fix valid ones and
rerun the full gate after fixes.

After review, run the GUI/backend scenario smoke against fresh isolated vdev
state: connect, keepalives, ARM/DISARM, active/cleared fault acknowledgement,
safe mode, delayed replies, stalled link and reboot. Inspect shell screenshots
in both themes and resolutions. Log the vdev run in `plans/hardware-log.md`.
H2a-2 requires vdev only; no physical board, motors, IMU or Pi check is added.

## Documentation and handoff

Update `host/gui/README.md`, `host/README.md`, `host/vdev/README.md` and
`docs/development.md` for the app, backend contract, scenario controls,
screenshots and test commands. Update `host/CLAUDE.md` where the GUI lifetime
and Esc rules need clarification. Correct the stale ArmButton-owned Esc note
in `plans/gui-design.md` to the implemented shell/application ownership.

Update `plans/host-architecture.md` and the H2a brief for the approved
keepalive-only interpretation and already-armed observation behavior. Update
`plans/host-status.md` with the model exception, H2a-1 merge, results and H2b
handoff. No wire definition/version or firmware change is planned. Any newly
required wire, safety-rule or later-step change returns to the user first.

## Results (2026-11-16)

- Built the local app, shell, Qt backend, generic models, command planner,
  vdev controls and screenshot tooling. Full screen content stays in the
  original later steps. No wire definition/version or firmware source changed.
- Internal choices: application-owned dispatch receiver with callback-chain
  draining; initial session/metadata worker with bounded early status history;
  per-loop/capacity free-slot allocation; prefix-only managed handles with no
  value-command API; opt-in, sequenced local vdev control files. These contracts
  are documented in the GUI/vdev READMEs.
- Self-review and independent Codex review at high effort (required for safety
  and threading). Four valid findings were fixed and re-reviewed: a queued
  later DISARM after a failed first request; DISARM before session publication;
  the unbounded initial event drain; observer close cancelling an admitted
  DISARM. Follow-up found no remaining confirmed safety/close issue. No
  findings were rejected. Added lost-request/lost-ARM-reply and observer-close
  regressions, preserving mutation uncertainty and the earlier owner's groups.
- `python tools/validate.py full` passed all seven checks with zero compiler
  warnings and no omissions after the review fixes. Both host checks include
  the real-vdev Qt tests; Linux additionally covers Boot during command setup
  over a retained PTY. The clean committed revision is recorded by the final
  validator summary in the PR.
- After review, `scenarios.py --backend ...` passed all 18 Windows QtTest
  checks and all ten named shell scenarios (40 PNGs, dimensions verified).
  Representative Connect, dialog, safe mode, fault/drawer and lost-link
  captures were inspected in both themes and resolutions: no clipping; scrim
  starts below the status bar and DISARM stays live. No physical board, motor,
  IMU or Pi was opened or changed; no new deferred hardware row is needed.

Validation observations to retain if they recur:

- Windows CI hit `stats missing` in `existingInputAndUncertainPartialSetup`:
  the fixture assumed the vdev's 100 ms poll would always finish within a fixed
  150 ms sleep. It now waits up to eight seconds for the exact existing sequenced
  `SCENARIO` acknowledgement, emitted after stats close, before reading the file
  or issuing the next injection. Fixture failures throw so cleanup unwinds instead
  of aborting. A dead-vdev regression checks that path.
- That CI run printed CTest's final failure summary but stayed running for almost
  six hours. The validator's pipe reader could wait forever when a descendant
  retained stdout after CTest exited. A regression reproduces that condition;
  output is now tailed from a temporary file until command exit, preserving the
  nonzero status and final partial line. The original retaining process cannot
  be identified: GitHub published no Windows archive or artifact after cancellation.
  The Windows build/test/install step also has a 30-minute limit. These changes
  affect test/validation infrastructure only; no production mutation is retried.
  Independent high-effort Codex follow-up review found no actionable defect.
  The 49 Python tooling tests, focused Windows backend suite, and full seven-check
  gate passed after these fixes, including both host toolchains and all firmware
  builds. The clean committed source is recorded by the updated PR summary.
- One Windows OTA integration run got a TCP reset during the pipeline's
  restore-reboot call. The fresh-fixture focused integration rerun and later
  full checks passed. No automatic REBOOT/mutation retry was added.
- A GUI reconnect assertion failed once before it reported detailed refusal
  text; eight isolated reruns passed. The fixture was also corrected to SAVE
  its 10 Hz/1 s native benchmark settings across Boot, with a rate readback
  assertion. The original failure's exact cause was not retained.
- One unchanged native binary failed the configurable-loop test's
  `c.revert().ok()` assertion (`test_configurable_loops.cpp:246`). Its isolated
  case and the next full native suite passed without a source change or retry
  rule. The final full gate includes the successful complete native run.
