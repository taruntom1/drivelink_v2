# F5a-1 implementation plan

Status: approved on 2026-11-13; implementation in progress. Based on `origin/firmware_overhaul` at
`46fd141`. The user approved Codex planning/build, an independent Sol review
at high effort, true cascade control, fresh commands after a mode switch,
and splitting F5a into two PRs/sessions on 2026-11-13.

## Scope and split

This session implements **F5a-1: shared motor interfaces, the real controller
and realistic mocks**, in one PR into `firmware_overhaul`.

- Add `mod_motor` with pure actuator/feedback contracts and
  `controller.cascade_pid` (type `0x20`, as in the architecture example).
  Register the controller in both firmware and vdev.
- Make `actuator.mock` and `feedback.mock` implement those contracts. The
  controller uses exactly the same implementation with mocks and, later,
  real hardware modules.
- **F5a-2**, after this PR merges, adds `actuator.pwm_dir`,
  `feedback.quadrature`, hardware force/release, driver failure paths and
  instance setup on real pins. Its plan must reconcile the architecture's
  reference to two MCPWM generators with the existing PWM+DIR HAL's one
  PWM generator; forcing the DIR pin low is not a substitute for forcing PWM.
- **F5b** still deletes `motor_manager`, the legacy controller and motor wire
  entries, and runs the bench loop-stall campaign. F6 and later retain their
  existing scope. F5a-1 does not introduce actuator/controller sources.

## Changes and behavior

1. **Interfaces and ownership.** Add small, pure interfaces under
   `firmware/components/mod_motor/include/mod_motor/`, implemented by the
   controller and mock modules without RTTI or concrete mock casts. Keep
   simulation-only plant observations explicitly optional: a real actuator
   does not provide encoder measurements. Resolve wiring at startup, preserve
   creation dependencies and tick order **feedback → controller → actuator**,
   and fail startup for missing or incompatible required refs.

   Approved safety constraint: one controller owns an actuator
   at a time. A duplicate claim fails startup and therefore blocks ARM through
   the existing failed-instance rule. Claims unwind on startup failure and
   destruction. A controller-owned mock ignores its standalone duty input;
   its `drive` action refuses to override the owner. Standalone mock driving
   remains available when no controller owns it. Document and test both paths.

2. **True cascade.** Open-loop mode uses duty; velocity mode runs the velocity
   PID; position mode runs position PID → bounded velocity target → velocity
   PID → bounded duty. Expose generated `vel.*` and `pos.*` tuning parameters,
   derivative cutoffs, a positive velocity limit and a duty cap that includes
   zero in its output interval. Gains default to zero and mode to open-loop.
   Publish position, velocity, duty, velocity target, and both PIDs' error,
   P/I/D and integrator signals. Provide the loop action `reset_integrators`.
   Add a visible `waiting_for_command` signal for a mode awaiting a new command.

   Extract the reusable PID calculation into a pure, nonlogging kernel with
   explicit configuration, reset, output tracking/preset and bounded
   anti-windup. Preserve the legacy PID wrapper's API and existing behavior;
   no legacy controller tuning or mode behavior is otherwise improved.
   Share the pure count conversion/velocity estimator with the new modules,
   retaining a forwarding API for legacy callers until F5b. No new pure
   source includes IDF, logging, `sdkconfig.h` or `CONFIG_*` symbols.

3. **Bumpless mode transfer and fresh commands.** Stage a mode change through
   the existing param batch. Record its application boundary in the owning
   loop; initialize the new mode from current feedback when the controller
   next ticks. Hold the previous commanded duty for that transition tick,
   subject to the current output cap and any disarm/emergency stop. Preset
   controller state to track that output and suppress derivative kick.

   Initialize velocity to measured velocity, position to measured position,
   or open-loop duty to the previous duty. Ignore inactive stored setpoints.
   Subsequently accept only a value-bearing command for the active input
   received strictly after the mode application boundary. A command received
   after that boundary during the transition tick may be used on the next
   tick. Equal timestamps are conservatively stale. Keepalives, unrelated
   inputs and rejected frames do not satisfy this condition. Holding a target
   still requires the existing command watchdog; it grants no extra grace.

   Add optional, loop-owned command receipt metadata to input bindings and
   update it when `GroupService` applies a validated mailbox. This is internal
   metadata, with no new command bytes, RX work queue, locks or retry loops.
   ARM/DISARM clear receipts and controller histories: duty/velocity hold zero
   and position holds the measurement. A mode change while disarmed cannot
   preserve nonzero output into a later ARM.

4. **Realistic mock plant.** Add parameter-controlled deadband, duty saturation,
   a selectable one-tick delay and deterministic seeded noise. Defaults enable
   the one-tick delay and a small deadband; noise defaults off. Keep the exact
   first-order plant integration. Quantize feedback into signed encoder counts,
   expose `count`, and derive position and filtered velocity from those counts
   using configurable counts/revolution and gear ratio. Feedback samples the
   preceding actuator state. Reset clears queued delayed duty, noise state and
   estimator history. Emergency stop overrides queued duty immediately; it
   removes motor effort while the simulated plant can coast.

5. **Integration.** Add the new YAML and regenerate definitions/docs/hash;
   extend firmware, native and vdev source/registration lists. The host library
   and CLI continue discovering module descriptions from the device. Existing
   frame, descriptor and value encodings remain unchanged: core v5, diag v3
   and NVS formats remain current. New items use unused IDs. Any encoding,
   new detail code or additional safety-rule change requires a separate user
   decision before implementation.

## Tests and hardware

- Extend native PID/estimator tests and add motor-module tests: all modes,
  both cascade limits, saturation recovery, zero gains, quantization/count
  wrap, deadband, delay, reproducible noise, ordering, invalid wiring,
  duplicate ownership and direct-duty bypass attempts. Verify all six directed
  mode transitions at nonzero output, with realistic effects enabled.
- Deterministically test old queued commands, an unchanged numeric value in
  a genuinely fresh frame, transition-tick arrivals, keepalives, unrelated and
  rejected frames, repeated mode writes, disarmed mode changes, and rapid
  disarm/re-arm. Test emergency stop against queued plant duty and transition
  output; preserve the monitor's exact-arm-generation and unknown-receipt
  behavior. Run the existing real-time allocation/blocking guard on these paths.
- Extend host/vdev integration to discover/configure the controller, save and
  reload wiring/tuning, command and stream velocity/position, switch modes and
  demonstrate fresh-command gating and watchdog recovery on Windows and Linux.
- Add `host/test/motor_smoke.py` with TCP/serial transport and mock/real module
  selection. In F5a-1, dry-run and run its mock path on vdev and the bare board.
  F5a-2 completes real-module setup and dry-runs the real-selection path as far
  as the bare board permits; the actual motor run remains deferred to HW.
  Include the first-run procedure and explicit pass criteria for tracking,
  mode-transfer continuity and watchdog stop. F5b adds the loop-stall scenario.
- Run `python tools/validate.py quick` while building, then one sequential
  `full` run with every check available and passing. Extend existing suites;
  add a validator/CI check only if a separate hardware-free suite is introduced.
- After review, flash normal firmware on COM3, run controller/mock smoke and
  existing core/safety/source/host regression smoke, inspect COM4 stack marks
  (>1 KB free), and restore the original loops-only layout/settings. Stop and
  report a missing port or unresponsive board. Real pin setup belongs to F5a-2;
  there is no pin/LEDC readback self-check in this step.

## Review, docs and finish

Self-review the entire diff, then run an independent **Sol review at high
effort**, focused on control math, fresh-command boundaries, ownership and
emergency-stop races. Verify findings, fix confirmed issues and rerun the
required validation. Keep the authored diff near the repo's 4–5k-line review
limit; report any further split needed before expanding the approved scope.

Update `plans/firmware-architecture.md` and `plans/host-architecture.md` with
the agreed cascade/mode semantics and split; update `docs/protocol/`, core and
mock READMEs, the new `mod_motor` README, affected firmware/main/test/component
rules and host/vdev usage. Record the Codex/Sol exception, decisions, hardware
results and F5a-2 handoff in `plans/overhaul-status.md`. Root `CLAUDE.md` changes
only where its description/rules need to reflect the new modules.

Open one PR into `firmware_overhaul`, include the complete full-validation
summary, review outcome and hardware results, and record its link in status.
F5a-2 starts in a new session after this PR merges.
