---
name: overhaul-step
description: Runs one step of the DriveLink host overhaul from a fresh branch to a pull request into host_overhaul - orient, plan for the user's approval, build, validate, review, hardware check, status update and PR. Use whenever the user asks to start, continue, resume or do the next overhaul step, names a step id from plans/host-status.md (T0, H1a to H6, HW, or a split row such as H3a-1), or asks to implement the next part of the host plan, even without the word "overhaul". Takes an optional step id; without one it picks the next ready step. User guide steps (UG1 to UG3) use the guide-step skill instead.
---

# Run one overhaul step

One session takes one step from a fresh branch to one PR into `host_overhaul`. The user approves the plan before any code is written, because decisions about the wire protocol, safety rules and later steps are theirs. Use whatever this environment offers (plan mode, a task list, review agents); plain messages work too.

Terms used below: **the status file** is `plans/host-status.md`; **the brief** is the step's section in `plans/host-steps.md`.

Track progress with this checklist:

```
- [ ] 1 Orient: step, branch, reading, code
- [ ] 2 Plan approved by the user
- [ ] 3 Built; validate.py full gate passes
- [ ] 4 Self-review; independent review if required
- [ ] 5 Hardware check run, or deferred with a script and a row
- [ ] 6 Status, docs, commit, PR, PR link
```

## 1. Orient

1. Run `git fetch origin`, then choose the step:
   - the argument, if one was given;
   - else a step already `in progress` or `waiting on hardware`: resume it on its existing branch;
   - else the first `planned` step whose dependencies (the brief's "Depends on") are all `merged`.

   Stop and tell the user if a dependency isn't merged, or if the session sits on a branch with unrelated work. If an earlier step's PR has merged since the table was updated, mark it `merged` in this step's status edit.
2. For a new step, branch from `origin/host_overhaul` with the environment's branch prefix and the suffix `overhaul-<step id>`.
3. Read the "Host overhaul in progress" section of the root `CLAUDE.md`, the whole status file, the brief, and the architecture sections the brief names; add `docs/protocol/` and `plans/firmware-architecture.md` when the step touches the wire or the firmware. Then read the scoped `CLAUDE.md` of each area the step edits.
4. Read the code the step changes, starting with targeted searches. A read-only search helper may help in a wide area; give it a specific question and paths. It never writes code.
5. Apply "Models and effort" in the status file and any exception the user approved. If a required model or capability is unavailable, settle that with the user before planning.

## 2. Plan (approval point)

Present the plan (in plan mode if the environment has one) and wait for approval. Keep it short:

- **Scope:** what this PR does and what it leaves to later steps. If it would pass about 4–5k lines, propose a split into rows such as H3a-1 and H3a-2.
- **Changes:** the files and components touched.
- **Tests:** changed and new tests; every piece of pure logic gets host or native tests.
- **Hardware check:** the selected checks and why each exercises a change, any checks explicitly required by the brief, which checks are deferred and the scripts written for them, and anything the user must do.
- **Review:** self-review or independent review, from the brief's Review column, and why.
- **Deviations:** internal choices you will make and log; anything that changes the wire protocol, a safety rule or a later step, put to the user now.
- **Docs:** which of `docs/protocol/`, `docs/development.md`, `docs/bench.md`, the scoped `CLAUDE.md` files, `host/README.md` and component READMEs change.

Settle every deviation question before building.

## 3. Build

Build in this session, right after approval, on the model that planned it; don't hand the build to a subagent, because the planning context would be lost. Follow the scoped `CLAUDE.md` rules of every area touched; in firmware the real-time, error and allocate-at-setup rules matter most. Leave legacy code that a later step deletes alone unless it blocks this one.

Validate with `python tools/validate.py` from the repository root (guide: `docs/development.md`, "Validation"):

1. While iterating, run the smallest relevant test or `--only <check>`; use `quick` when its scope is relevant. After a fix, rerun the affected test/check first. Finish the implementation changes before starting the full gate.
2. Before review, run `full` once in the background and wait for it. Checks run one at a time because the timing tests fail under parallel load, so never start another build beside it. Passing checks whose inputs are unchanged are reused.
3. Read the outcome from `build/validate/summary.json`, not from console output. First confirm `finished_at` and `source` belong to your current revision, since the file describes the last completed run. Then read `full_gate_passed`, and for a failed check its `failure_reason` and `diagnostic_excerpt`; open its `log_path` only when the excerpt isn't enough. A `reused` check counts as passed.
4. Fix and repeat until `full_gate_passed` is true with nothing skipped.

Add tests to the existing host/native checks when appropriate. Create a new `Check` in `tools/validate.py` only for a distinct validation target or toolchain, and name it in the CI job that provides that toolchain.

When the plan turns out wrong while building: decide a small internal choice and log it in the status file's decision log. Stop and ask about anything touching the wire protocol, a safety rule or a later step, then update the plan document in this PR.

## 4. Review

Review your own diff (`git diff origin/host_overhaul...HEAD`) for correctness bugs, real-time violations, safety holes (arming, DISARM, the watchdog, the lease, command paths), protocol inconsistencies, missing tests and mismatches with the plan.

Run an independent review when the brief's Review column requires it, or when the diff is large or touches those safety paths beyond what your own review covers; the user may override either way. Use the environment's review agent with the model and effort from "Models and effort". Give it the diff base, the approved plan, the brief, the scoped rules of the areas touched, what you already verified, and the risky parts to focus on. One reviewer is the default; send it the changed diff and the unresolved findings as follow-ups, and widen the review only when a fix breaks an earlier assumption. If a required review can't run, tell the user before going further.

Check every finding against the code. Fix those that hold, rerun `validate.py full`, and note rejected findings with their reasons for the PR.

## 5. Hardware check

After review, run the checks selected in the approved plan. Keep checks explicitly required by the brief; select additional checks to exercise the changed behavior. Reuse prior results for other unchanged paths, and broaden coverage when shared transport, protocol, persistence, timing or safety changes justify it. Read `docs/bench.md` before board checks.

- **Bench board** (mock modules, smoke tests, HIL, OTA, bench builds): the session runs the selected checks on the existing firmware. Flash only when the firmware/configuration changed or the check requires another image; use `tools/bench_fw.py` for bench and normal images. Restore the board to "Board state" in the status file afterwards, including the original firmware when changed, and confirm it refuses bench commands. Judge timing only from device timestamps and counters, because the CH343 delivers status frames late. If a port is missing or the board doesn't answer, stop and tell the user.
- **Motors, a real IMU, the Pi or the robot:** deferred. Write the script with a mock mode, dry-run it on vdev or the bench board, and add its row to "Deferred hardware checks" in the status file. The step doesn't wait.
- **A check that needs the user's hands** (wiring, a Pi, a one-time admin command): set the step to `waiting on hardware` and give the user the exact build and flash commands, what to run, and what result means pass. Wait for their result.

Log each run as one row in `plans/hardware-log.md`, and update "Board state" if the board changed. On a failure, fix and repeat.

## 6. Finish

1. Update the status file: the step's row to `in review`; replace "Next step" and the handoff with what the next session needs (traps, unfinished edges); add decision rows, and drop rows that the plans or docs now state.
2. Update the docs named in the plan, now that the results are known.
3. Commit with a message saying what changed and why, then push.
4. Open the PR with `gh pr create --base host_overhaul`. The description covers the step and what was done; the text of `build/validate/summary.txt` from the last `full` run, once `summary.json` shows `full_gate_passed` for the pushed revision; the deviations; the review (self or independent, why, and rejected findings); the hardware result.
5. Put the PR link in the step's row, then commit and push.
6. Tell the user the PR link and anything they must do. The next step starts in a new session after the merge.

## Working efficiently

A step is long, so keep context lean without weakening any gate above:

- Read a file once; afterwards use targeted searches, excerpts and diffs. Keep a short working record (approved decisions, invariants, paths, open issues, checks done, next action) instead of rebuilding the task.
- Send long build, flash and test output to logs, and read summaries and failure excerpts.
- While a build, test, review or hardware script runs, keep settled decisions fixed. Continue useful independent work; otherwise use one bounded wait of up to 60 seconds, then inspect only new output. Reopen analysis when a failure, new evidence or user input requires a decision. Keep progress updates brief.
- Debug from the smallest evidence that discriminates (timestamps, state, failure reason, the relevant log lines). Capture transient evidence before cleanup when that is safe; stopping unsafe behaviour comes first.
- Put multi-line edits and scripts in files, not shell heredocs.
- Write docs and the status file once, after review and hardware results.
- If the user sets a soft token target, report usage, remaining work and ways to narrow scope when it is reached. Never cut verification to save tokens, and honour a hard limit.
