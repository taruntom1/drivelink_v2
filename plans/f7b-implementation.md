# F7b implementation plan (approved 2026-11-14)

Test signal generation, integrated with capture, over core wire v7. The user
approved this plan before implementation. One PR targets `firmware_overhaul` from
`codex/overhaul-F7b`; F7a is merged in the branch's base.

## Decisions already approved

- Codex plans and builds F7b, with an independent Codex review at high effort.
- One generator per running loop; different loops may run independently.
- Completion and STOP retain the last generated target value until a fresh
  command arrives. Commands for that target received during the test are
  discarded, including mailboxes not consumed before the test ends.
- Command frames must keep feeding the existing watchdog. DISARM stops every
  generator and retains the existing actuator emergency-stop behavior.

## Scope

Implement all planned waveforms, START/STOP/STATUS, the capture
`testgen_start` trigger, TestgenDone events and the `testgen_running` status
flag. Add matching SDK, CLI, vdev, native and host coverage, and a shared
mock/real-motor smoke script. Evaluate `SPIRAM_XIP_FROM_PSRAM` on the bare
board and record its timing and memory tradeoffs.

Time synchronization, OTA, plant identification fitting, GUI work and the
physical motor campaign remain in their existing later steps. No new task or
persistent generator configuration is needed. Aim for one reviewable PR; if
the implementation grows beyond the repository's roughly 4–5k-line split
threshold, propose a split before expanding it.

## Generator and command ownership

Add a pure `TestgenService`, owned by Core, with fixed per-loop storage
allocated before loops start. The config caller validates START and resolves
the target through the existing binding metadata. Targets must be running,
same-loop f32 inputs carrying the Testgen flag; these cover every currently
flagged controller and mock input. Other types/kinds are refused explicitly.

Transfer START and STOP to the owning loop through a bounded atomic
handshake. The loop never waits, allocates or logs. A second START on an
active generator is refused without disturbing it; STOP is idempotent.
Acknowledgements and timeouts distinguish an applied request from one
definitely withdrawn, following the existing staging contracts.

Apply generation after staged params/actions and command-group consumption,
before instance ticks. The target's value and optional receipt timestamp are
written only on its owning loop. Each generated sample supplies a receipt
from the actual write time, so a staged controller mode switch can satisfy
its strictly-after-boundary freshness check on a subsequent tick. Generation
never feeds the watchdog by itself.

Suppress command-group writes and receipts for the active target while
preserving other entries in the same command frame and the frame's watchdog
receipt. On termination, retain a per-target handoff boundary that rejects
commands received at or before termination, even if their mailbox was busy
until afterward. A new generator must not erase a previous target's pending
fresh-command requirement. Keepalives do not change the held value or make
it fresh. ARM/DISARM retain the existing input reset contract.

Tie activation to the exact arm generation: a concurrent disarm or a rapid
disarm/re-arm cancels old requests/runs, and nothing restarts automatically.
The monitor's emergency force remains independent of loop progress. STATUS
and the aggregate running bit must not report an old-arm run as active.

## Waveforms and timing

Use device elapsed time from the first emitted sample, not host timing or
an assumption that every scheduled tick ran. Each run has finite offset and
amplitude, a positive duration, applicable frequencies, and a repeat count
(1 means once, 0 means repeat until STOP/DISARM). Duration is per repetition;
repetition restarts waveform phase without another capture-start trigger.

- Step: `offset + amplitude` from the first emitted sample.
- Square: 50% duty, alternating `offset + amplitude` and `offset - amplitude`,
  starting high.
- Ramp: from offset toward `offset + amplitude` over the duration.
- Sine: offset plus amplitude times sine, starting at zero phase.
- Linear/log chirp: zero-phase sine with frequency swept from start to end
  over the duration; log chirp requires both frequencies positive.

Define endpoint sampling, equal-frequency chirps and repeat boundaries in
the wire documentation and deterministic tests. Reject malformed/nonfinite
parameters and invalid frequency/duration combinations before mutation;
require applicable frequencies below the owning loop's Nyquist limit.
Use numerically bounded phase evaluation and clamp every generated output
to the target descriptor's range. The last emitted sample is the value held
on completion; no extra endpoint write is hidden after completion.

## Wire, events and capture

Bump core v6 to v7 and update firmware, host and the Python protocol reference
together. Add generated Testgen op, waveform, state and terminal-reason enums
under family 7. START carries loop, target Ref, waveform and its parameters;
STOP and STATUS select a loop. STATUS returns the accepted configuration,
run identity, state, progress and terminal reason. START returns a run
identity so events from earlier runs can be distinguished.

Add TestgenDone as the next CoreEvent id, with loop, run identity and reason
(completed, stopped or disarmed). Publish fixed terminal snapshots from the
loop and send events from the config task, never the control tick. Status
bit 16 means at least one generator is effectively running. Document exact
field widths, lengths, validation and refusal details in `docs/protocol/core.md`.

Accept capture's numbered `testgen_start` trigger. The generator's first
emitted sample requests the trigger only on the same loop; the next due
recorded sample consumes it, including when capture uses a divider. A start
on another loop or a later capture ARM does not retroactively trigger.
Capture continues after generation ends or the device disarms, preserving
the response and stop history.

## Host surface

Add libdrivelink testgen methods and strict response/event decoding, plus
CLI `testgen-start`, `testgen-stop`, `testgen-status` and `testgen-wait`.
Provide a bounded CLI run workflow that starts an already configured test,
feeds a selected command group with keepalives, and disarms on exit/failure;
service START alone never silently arms or supplies watchdog feeds.
Waits correlate loop/run identity, poll STATUS and tolerate a late/missed
event as capture waits do. vdev uses the actual new core service and forwards
its events and running flag.

## Files and verification

Touch core service/loop/group/capture/command code and IDF CoreLink, the
registry and core codec, generated outputs, host library/CLI/vdev, their
CMake source lists and tests. Add `test_core_testgen.cpp` and
`host/test/testgen_smoke.py`; extend the existing host integration suite so
it remains covered by the Windows/Linux validator checks. Any standalone
new suite also gets a named validator check and CI invocation.

Native tests cover waveform values and boundaries, repetition, clipping,
invalid requests without side effects, exact wire lengths, start/stop
handshakes and withdrawal, run/event identities, sparse and concurrent loops,
capture trigger/divider alignment, command suppression and fresh handoff
(including delayed mailboxes and changing targets), controller mode freshness,
watchdog behavior, arm-generation races and the real-time guard.

Host tests cover encoding/decoding, malformed replies/events, CLI parsing,
wait races, and a captured step and chirp on vdev. Run focused checks during
implementation, then `python tools/validate.py full` with nothing omitted.
Self-review and one independent Codex review at high effort are required
because this touches wire, concurrency and armed input ownership. Verify and
fix findings; rerun full validation after fixes, before hardware checks.

## Bare board and PSRAM evaluation

After review, flash normal firmware through `tools/bench_fw.py normal`.
Run the new smoke script on mocks at 1 kHz: capture generated input and motor
response for a step and chirp, verify timing with captured loop counters,
exercise STOP/hold/fresh-command handoff, DISARM and watchdog cancellation,
and check all task stack marks exceed 1 KB. Run affected existing smoke
checks and restore the original loops, rates, watchdog, layout and groups.

Compare otherwise identical XiP-off/on builds with 1-ref and 16-ref capture,
and active chirp generation. Record device tick costs, overruns, irregular
capture, available capture memory, and disarmed flash-write behavior. Keep
XiP in an isolated build/configuration, adding a guarded helper variant if
needed, and restore normal firmware afterward. Keep XiP disabled by default
in this PR; present any recommendation to enable it with measured results
and boot/fallback implications for a separate user decision. Existing
SAVE/OTA armed restrictions do not change.

The same testgen smoke script gets a real-module mode with the existing
first-motor-run procedure and explicit duty/gain/tolerance arguments. Dry-run
the workflow with mocks now and add its real step/chirp command and pass
criteria to Deferred hardware checks. No motors or IMU are currently attached.

## Documentation and completion

Update `docs/protocol/`, the architecture's testgen contract, root/firmware/
component rules where behavior changes, the core and host READMEs, and the
overhaul status file. Record the approved Codex exception, terminal/handoff
policy, F7a's merged state, XiP results, hardware results and the F8 handoff.
No unrelated legacy changes. After all gates pass, commit, push and open one
PR into `firmware_overhaul`, then record its link in the status file.
