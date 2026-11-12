---
name: overhaul-step
description: Run one step of the DriveLink firmware overhaul (S1, F1a ... F10, H1 ...) from start to PR. Use when the user says to start, continue or do the next overhaul step, or names a step id from plans/overhaul-status.md. Optional argument is the step id; without one, take the next planned step.
---

# Run one overhaul step

One session runs one step and ends with one PR into `firmware_overhaul`. Follow these stages in order, and don't skip the approval points.

**Roles.** The session is the planning agent (Opus 5.5, high). It plans, delegates the build to an implementation subagent, reviews the result itself and runs bare-board hardware checks. There is no separate review agent.

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
4. Read the step's row for **Build model · effort** and **Review effort**, and the "Models and effort" rules in the status file. The session must run on Opus 5.5 at high effort. If it doesn't, tell the user to switch before planning, then continue.
5. Read the code the step will change before planning. For wide searches, use an `Explore` agent with `model: "haiku"`. Search agents only read; they never write code.

## 2. Plan (approval point)

Enter plan mode and write a short step plan that the implementation agent can follow on its own:
- **Scope:** what this PR does, and what it leaves for later steps. If the step is too big for one reviewable PR, propose splitting it into rows such as `F3a-1` and `F3a-2`.
- **Changes:** the files and components touched.
- **Tests:** which existing tests change, and which new ones are added (host or native tests for every piece of pure logic).
- **Hardware check:** none, bare board (you run it) or attached hardware (the user runs it), and what counts as a pass.
- **Plan deviations:**
  - Internal choices you will make and log.
  - Anything that changes the wire protocol, a safety rule or a later phase. These need the user's decision now.
- **Docs:** `docs/protocol/`, `CLAUDE.md`, `firmware/CLAUDE.md`, `firmware/components/CLAUDE.md`, READMEs of new components, and the status file.

Wait for the user's approval. Answer any deviation questions before you build.

## 3. Build (implementation subagent)

After approval, spawn one `general-purpose` agent with the step's build model and effort. Its prompt carries the approved plan, the relevant plan sections and the rules below.

- Follow the root `CLAUDE.md`, `firmware/CLAUDE.md` and `firmware/components/CLAUDE.md`, especially the real-time rules, the error rules and the "allocate only at setup" rule.
- Don't improve legacy code that a later phase deletes, unless it blocks this step.
- Commit locally. Don't push, open the PR, edit the status file or touch the board.
- If the plan turns out wrong or the step needs a change the plan doesn't cover, stop and report.
- Before reporting, all of these must pass with zero warnings (the PowerShell recipe for exporting IDF is in `firmware/CLAUDE.md`):
  - `idf.py -C firmware build`
  - the nolog variant (from inside `firmware/`: `idf.py -B build_variant -D SDKCONFIG=build_variant/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.nolog" build`)
  - `idf.py -C firmware/test/uart_comm build`
  - `python tools/gen/gen.py --check`
  - `python -m unittest discover -s tools/gen/tests`
  - the native tests (`cmake -S firmware/test/native -B firmware/test/native/build -G Ninja`, `cmake --build firmware/test/native/build`, `ctest --test-dir firmware/test/native/build --output-on-failure`), plus any suites the step introduces
- Report: what changed, check results, internal choices to log, anything unfinished.

Then:
- Check the report against the plan and the commits.
- Decide small internal choices and log them. Wire protocol, safety rule or later-phase changes go to the user, and the plan is updated in this PR.
- Send rework to the same agent with `SendMessage`.

## 4. Review (planning agent)

Review `git diff origin/firmware_overhaul...HEAD` yourself at the step's **Review effort**. If it is `xhigh`, raise the session effort for the review. Look for correctness bugs, real-time violations, safety holes, protocol inconsistencies, missing tests, mismatches with the plans, and scope creep.

Fix what holds up (small fixes yourself, larger ones through the implementation agent), rerun the checks, and note any findings you rejected, with the reason, for the PR description.

## 5. Hardware check (only if the step needs one)

Run it after the review. Record it in the status file's hardware table, with the date, the step, the board, what was run and the result. Fix and repeat if it fails.

- **Bare board:** build, flash and run it yourself, using the ports in the status file's hardware notes. If a port is missing or the board doesn't respond, stop and tell the user.
- **Attached hardware** (motors, real IMU, robot, Pi): don't flash. Set the state to `waiting on hardware` and give the user a short script: what to build and flash, what to run, and what means pass or fail. Wait for their result.
- Never run anything that could move an actuator. If you can't tell what is attached, treat it as attached hardware.

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
