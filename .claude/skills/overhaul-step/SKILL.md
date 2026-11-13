---
name: overhaul-step
description: Run one step of the DriveLink firmware overhaul (S1, F1a ... F10, H1 ...) from start to PR. Use when the user says to start, continue or do the next overhaul step, or names a step id from plans/overhaul-status.md. Optional argument is the step id; without one, take the next planned step.
---

# Run one overhaul step

One session runs one step and ends with one PR into `firmware_overhaul`. Follow these stages in order, and don't skip the approval points.

## 1. Orient

1. Run `git fetch origin`. Create a fresh branch from `origin/firmware_overhaul`, named `claude/overhaul-<step id>` (for example `claude/overhaul-f1a`). If the session already sits on a branch with unrelated work, stop and tell the user.
2. Read these:
   - the "Firmware overhaul in progress" section of the root `CLAUDE.md`
   - all of `plans/overhaul-status.md`
   - the parts of `plans/firmware-architecture.md` and `plans/host-architecture.md` that the step touches
3. Pick the step:
   - the argument, if one was given
   - otherwise the first step in the table whose state is `planned`, provided every step it depends on is `merged`
   - If a step it depends on isn't merged, stop and tell the user.
4. Read the step's **Review effort** and the "Models and effort" rules in the status file. The session plans **and** builds the step, on Opus 5.5 at high effort. If the session runs on a different model or effort, tell the user to switch before planning (in the app's model picker), then continue.
5. Read the code the step will change before planning. For wide searches, use an `Explore` agent with `model: "haiku"`. Search agents only read; they never write code.

## 2. Plan (approval point)

Enter plan mode and write a short step plan:
- **Scope:** what this PR does, and what it leaves for later steps. If the step is too big for one reviewable PR, propose splitting it into rows such as `F3a-1` and `F3a-2`.
- **Changes:** the files and components touched.
- **Tests:** which existing tests change, and which new ones are added (host or native tests for every piece of pure logic).
- **Hardware check:** needed or not. If it is, say what the session runs on the bare board, which checks are deferred (motors, IMU, robot) and the scripts written for them, or what the user will be asked to run.
- **Review:** whether an independent review agent is needed (see stage 4), and why.
- **Plan deviations:**
  - Internal choices you will make and log.
  - Anything that changes the wire protocol, a safety rule or a later phase. These need the user's decision now.
- **Docs:** `docs/protocol/`, `CLAUDE.md`, `firmware/CLAUDE.md`, `firmware/components/CLAUDE.md`, READMEs of new components, and the status file.

Wait for the user's approval. Answer any deviation questions before you build.

## 3. Build

The session builds the step itself, right after the approval, with no model switch. Don't hand the build to a subagent. Search agents (Haiku, read-only) may still help find code.

- Follow the root `CLAUDE.md`, `firmware/CLAUDE.md` and `firmware/components/CLAUDE.md`, especially the real-time rules, the error rules and the "allocate only at setup" rule.
- Don't improve legacy code that a later phase deletes, unless it blocks this step.
- Validate with `python tools/validate.py` from the repository root (the guide is in `firmware/test/CLAUDE.md`, "Validation"):
  - While building, run `quick` (generator, native tests, Windows host tests), or one check with `--only`.
  - Before moving on, `full` must pass with nothing skipped: the three zero-warning IDF builds, the generator checks, the native tests and the host tests on Windows and Linux.
  - It runs the checks one at a time and sets up each toolchain itself. Start it once, in the background, and wait for it. Never start a second build or test run beside it.
  - A rerun skips the checks whose code is unchanged, so after a fix just run `full` again.
  - A test suite the step introduces becomes a `Check` in `tools/validate.py`, and CI calls it by name.
- If something in the plans turns out wrong while building:
  - A small internal choice: decide, and log it in the decision log.
  - Anything that changes the wire protocol, a safety rule or a later phase: stop and ask the user, then update the plan document in this PR.

## 4. Review

First review the diff yourself (`git diff origin/firmware_overhaul...HEAD`). Look for correctness bugs, real-time violations, safety holes, protocol inconsistencies, missing tests, and mismatches with the plans.

Then decide whether an **independent review agent** is needed. Spawn one when the step changes the wire protocol, a safety path (arming, the monitor, the watchdog, `emergencyStop`, disarm triggers), persistence or concurrency in a way your own review can't fully cover, or when the diff is large. Skip it for docs-only, tooling or mechanical steps. Say which you chose, and why, in the PR description. If the user says to skip it (or to run it), do that.

When you spawn one: a `general-purpose` agent on `git diff origin/firmware_overhaul...HEAD`. Always pass `model: "opus"`, and pass `effort` set to the step's **Review effort** from the status table (`high` or `xhigh`). This skill is the instruction that sets that effort. Give the agent:
- the step plan and the relevant plan sections
- the rules in the root `CLAUDE.md`, `firmware/CLAUDE.md` and `firmware/components/CLAUDE.md`
- the brief above

Verify each finding, yours or the agent's, against the code. Fix the ones that hold up, run `python tools/validate.py full` again, and note any you rejected, with the reason, for the PR description.

## 5. Hardware check (only if the step needs one)

- **Bare bench board** (nothing attached: boot checks, HIL pytest, mock modules, smoke tests, OTA): the session builds, flashes and runs it itself, after the review. Stop and tell the user if a port is missing or the board doesn't respond. The board's ports are in the status file's hardware notes.
- **Motors, a real IMU or the robot:** deferred, because the bench has none. Follow "Deferred hardware checks" in the status file: write the script, dry-run it against vdev or the bare board with mocks, and add its row there. The step doesn't wait for it.
- **A Pi or a PC** (or deferred hardware the user says has arrived): don't run it yourself. Set the step to `waiting on hardware`, and give the user a short script:
  - what to build and flash, with exact commands
  - what to run
  - what output or behavior means pass or fail

  Wait for their result.

Record the result in the status file's hardware table, with the date, the step, the board, what was run and the result. Fix and repeat if it fails.

## 6. Finish

1. Update `plans/overhaul-status.md`:
   - the step's row: state `in review`
   - "Next step" and the handoff note: what the next session must know, traps, unfinished edges
   - any decision log rows
2. Update the docs listed in the plan.
3. Commit with a message that says what changed and why, then push.
4. Open the PR with `gh pr create --base firmware_overhaul`. The description covers:
   - the step id and what was done
   - the validation: the text of `build/validate/summary.txt` from the last `full` run
   - the plan deviations
   - the review: self-review only or with a review agent (and why), and the findings you rejected
   - the hardware result
5. Put the PR link in the step's row, then commit and push that.
6. Tell the user the PR link and anything they must do. The next step starts in a new session after this PR is merged. The next session sees the merge and records the state as `merged`.
