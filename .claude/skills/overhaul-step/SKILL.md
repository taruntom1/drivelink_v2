---
name: overhaul-step
description: Run one step of the DriveLink firmware overhaul (S1, F1a ... F10, H1 ...) from start to PR. Use when the user says to start, continue or do the next overhaul step, or names a step id from plans/overhaul-status.md. Optional argument is the step id; without one, take the next planned step.
---

# Run one overhaul step

One session runs one step and ends with one PR into `firmware_overhaul`. Follow these stages in order, and don't skip the approval points.

## 1. Orient

1. Run `git fetch origin`. Create a fresh branch from `origin/firmware_overhaul`, named `claude/overhaul-<step id>` (for example `claude/overhaul-f1a`). If the session already sits on a branch with unrelated work, stop and tell the user.
2. Read these:
   - the "Firmware overhaul in progress" section of `CLAUDE.md`
   - all of `plans/overhaul-status.md`
   - the parts of `plans/firmware-architecture.md` and `plans/host-architecture.md` that the step touches
3. Pick the step:
   - the argument, if one was given
   - otherwise the first step in the table whose state is `planned`, provided every step it depends on is `merged`
   - If a step it depends on isn't merged, stop and tell the user.
4. Read the code the step will change before planning.

## 2. Plan (approval point)

Enter plan mode and write a short step plan:
- **Scope:** what this PR does, and what it leaves for later steps. If the step is too big for one reviewable PR, propose splitting it into rows such as `F3a-1` and `F3a-2`.
- **Changes:** the files and components touched.
- **Tests:** which existing tests change, and which new ones are added (host or native tests for every piece of pure logic).
- **Hardware check:** needed or not. If it is, say what the user will be asked to run.
- **Plan deviations:**
  - Internal choices you will make and log.
  - Anything that changes the wire protocol, a safety rule or a later phase. These need the user's decision now.
- **Docs:** `docs/protocol/`, `CLAUDE.md`, `components/CLAUDE.md`, READMEs of new components, and the status file.

Wait for the user's approval. Answer any deviation questions before you build.

## 3. Build

- Follow `CLAUDE.md` and `components/CLAUDE.md`, especially the real-time rules, the error rules and the "allocate only at setup" rule.
- Don't improve legacy code that a later phase deletes, unless it blocks this step.
- Before moving on, all of these must pass with zero warnings:
  - `idf.py build`
  - the nolog variant (`-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.nolog"` in a separate build dir)
  - `idf.py -C test/uart_comm build`
  - `python tools/gen_diag_codes.py --check`
  - the host tests

  Use the paths that are current after S1. The PowerShell recipe for exporting IDF on this machine is in `CLAUDE.md`. Add any test suites the step introduces (native tests, vdev, host superbuild).
- If something in the plans turns out wrong while building:
  - A small internal choice: decide, and log it in the decision log.
  - Anything that changes the wire protocol, a safety rule or a later phase: stop and ask the user, then update the plan document in this PR.

## 4. Review

Spawn an independent review agent (`general-purpose`) on `git diff origin/firmware_overhaul...HEAD`. Give it:
- the step plan and the relevant plan sections
- the `CLAUDE.md` rules
- a brief to find correctness bugs, real-time violations, safety holes, protocol inconsistencies, missing tests, and mismatches with the plans

Verify each finding against the code. Fix the ones that hold up, rerun the checks, and note any you rejected, with the reason, for the PR description.

## 5. Hardware check (only if the step needs one)

Don't flash the board yourself. Stop and give the user a short script:
- what to build and flash, with exact commands
- what to run
- what output or behavior means pass or fail

Wait for their result. Record it in the status file's hardware table, with the date, the step, the board, what was run and the result. Fix and repeat if it fails.

## 6. Finish

1. Update `plans/overhaul-status.md`:
   - the step's row: state `in review`
   - "Next step" and the handoff note: what the next session must know, traps, unfinished edges
   - any decision log rows
2. Update the docs listed in the plan.
3. Commit with a message that says what changed and why, then push.
4. Open the PR with `gh pr create --base firmware_overhaul`. The description covers:
   - the step id and what was done
   - the plan deviations
   - the review findings you rejected
   - the hardware result
5. Put the PR link in the step's row, then commit and push that.
6. Tell the user the PR link and anything they must do. The next step starts in a new session after this PR is merged. The next session sees the merge and records the state as `merged`.
