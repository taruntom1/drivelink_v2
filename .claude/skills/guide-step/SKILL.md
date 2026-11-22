---
name: guide-step
description: Runs one step of the DriveLink user guide (the GitHub Pages website for people who use only the released artifacts) from a fresh branch to a pull request. It covers orienting, planning for the user's approval, building pages and site tooling, trying every command, validating, reviewing, browser checks, the status update and the PR. Use it whenever the user asks to start, continue, resume or do the next user guide step, names a step id from plans/guide-status.md (UG1, UG2, UG3, or a split row such as UG2-1), or asks to work on the user guide, the docs website or the Pages site. Takes an optional step id. Without one, it picks the next ready step.
---

# Run one user guide step

One session takes one step of the user guide from a fresh branch to one PR. The user approves the plan before any file changes, because the guide's decisions (audience, structure, site, publishing) are theirs.

The guide is for a reader with only the released artifacts and no ROS or embedded experience. Judge every choice by whether that reader succeeds. The guide describes the product as it is. When the product is wrong or missing something, report it. Do not change it in a guide step.

Terms used below:
- **The status file** is `plans/guide-status.md`.
- **The plan** is `plans/guide-steps.md`: its rules, its decisions and the step's brief.
- **The writing rules** are in `guide/CLAUDE.md`. UG1 creates that file; until then, the plan's UG1 brief lists them.

Use whatever the environment offers: plan mode, a task list, review agents or a browser. Plain messages and shell commands work too.

Copy this checklist and keep it current:

```
- [ ] 1 Orient: step, branch, reading
- [ ] 2 Plan approved by the user
- [ ] 3 Built; every command and GUI step tried; validate.py full passes
- [ ] 4 Self-review as the reader; independent review if required
- [ ] 5 Browser checks; bare-board check if the brief names one
- [ ] 6 Status, docs, commit, PR, PR link
```

## 1. Orient

1. Run `git fetch origin`, then choose the step:
   - the argument, if the user gave one;
   - otherwise a step that is `in progress` or `waiting on hardware`, resumed on its existing branch;
   - otherwise the first `planned` step whose "Depends on" steps are all `merged`. A dependency outside the guide, such as H4d, is checked in `plans/host-status.md`.

   Stop and tell the user if a dependency isn't merged, or if the working tree has unrelated changes.
2. For a new step, create a branch from the integration branch that the status file's "Branches" line names. Use the environment's branch prefix and the suffix `guide-<step id>`.
3. Read these:
   - the root `CLAUDE.md`, including any temporary section such as "Host overhaul in progress";
   - the whole status file;
   - the plan's rules, its decisions and the step's brief;
   - the writing rules;
   - the "guide" parts of `docs/development.md`, once they exist.

   Then read the product docs behind the pages you will write: the GUI, agent, bringup and hardware-plugin READMEs, `docs/pi-setup.md` and `docs/protocol/` as needed. When you try commands in WSL, Docker or the ROS containers, also read the "Traps" in `plans/host-status.md`.
4. Read only the code that the site tooling or the screenshot scenarios touch. To find where a feature lives, search for it, or give a read-only helper a specific question and specific paths.
5. Follow "Models and effort" in the status file. If a required capability is missing (for example an independent reviewer or a browser), settle it with the user before planning.

## 2. Plan (approval point)

Present the plan (in plan mode if the environment has one) and wait for approval. Keep it short:

- **Scope:** what this PR does and what it leaves to later steps. If the step would pass about 4–5k lines (vendored and generated files excluded), propose a split.
- **Pages:** each page you will add or change, with one line on what the reader can do after reading it.
- **Tooling:** the changes to the build, the checks, CI, publishing and the screenshot scenarios, with the files they touch.
- **Trying it:** how each command and GUI step will be run (vdev, the agent, the ROS containers, WSL), and which screenshots will be rendered.
- **Checks:** the browser checks and any bare-board check the brief names, with what the user must do or allow.
- **Review:** self-review or independent review, as the brief says, and what the reviewer should focus on.
- **Questions:** anything that would change a decision in the plan, the release process or the product. These are the user's to decide. Smaller internal choices go in the decision log.
- **Docs:** the writing rules, `docs/development.md`, `packaging/apt/README.md`, and the repository docs whose user text moves.

Settle every question before building.

## 3. Build

Build in this session right after approval. Planning and building stay in one context.

- **Write for the reader.** Follow the writing rules. Every page states what the reader needs before starting and what they will have at the end. After a step, say what the reader should see. Explain a term or link it to the glossary the first time it appears on a page. Put a safety callout before anything that can move a motor. The reader cannot see the source, so describe what the product does, never how the code does it.
- **Try what you write.** Run every command, and walk through every GUI step, against the released or CI-built packages, as the plan's rules describe. Copy labels, messages and values from the running product, not from memory or source. If something behaves differently from the docs, the product wins. Tell the user, and log it in "Product issues found" when it is a bug.
- **Screenshots** come only from the GUI's scenario tooling, so they can be regenerated. Add a scenario before you add a picture.
- **Validate** with `python tools/validate.py`, as `docs/development.md` ("Validation") describes:
  1. While iterating, use `--only guide` or the smallest check that covers the change.
  2. Before review, run `full` once and wait for it. Its checks run one at a time because the host timing tests fail under parallel load, so start no other build beside it.
  3. Read the result from `build/validate/summary.json`, not from the console.
     - First confirm that `finished_at` and `source` match your revision.
     - Then read `full_gate_passed`, and the `failure_reason` and `diagnostic_excerpt` of any failed check.
     - A `reused` check counts as passed.
  4. Fix and repeat until `full_gate_passed` is true and nothing was skipped.

If the plan proves wrong while you build, decide small internal choices and log them in the decision log. Stop and ask before changing anything listed under "Questions" in the plan.

## 4. Review

Review your own diff against the integration branch.

- **As the reader.** Walk each journey you touched on the built site, using only the guide and the released artifacts. Look for:
  - a step the reader cannot do from the page alone;
  - an unexplained term;
  - a missing "you should see";
  - a command that assumes the source;
  - a missing safety callout;
  - a screenshot that disagrees with the text;
  - a page that reads like a developer README.
- **As an engineer.** Look for:
  - in the build and check code: bugs and missing tests;
  - in CI and publishing: permissions, where secrets are read, what is pushed where, and that the apt paths are unchanged;
  - no external URLs for scripts, styles or fonts;
  - links and anchors that resolve;
  - accessibility: alt text, contrast from the tokens, keyboard use.

When the brief requires an independent review, run one, and run one also when the diff is large or touches publishing beyond what your own review covers. The user may override either way.
- **Reviewer:** a separate session or agent at the effort set in "Models and effort".
- **Give it:** the diff base, the approved plan, the brief and the plan's decisions, the writing rules, what you already verified, and the parts to focus on.
- **Follow-ups:** send it the changed diff and the unresolved findings. If it can't run, tell the user before going further.

Check every finding against the site and the code. Fix the ones that hold and rerun `validate.py full`. Note rejected findings and their reasons for the PR.


Log each bare-board run as one row in `plans/hardware-log.md`.

## 5. Finish

1. Update the status file:
   - set the step's row to `in review`;
   - replace "Next step" and the handoff with what the next session needs;
   - add decision rows, and remove rows that the plan or the docs now state;
   - keep "Product issues found" current.
2. Update the docs named in the plan.
3. Commit with a message that says what changed and why, then push.
4. Open the PR into the integration branch. Its description covers:
   - the step and the pages it adds;
   - the text of `build/validate/summary.txt` from the last `full` run, once `summary.json` shows `full_gate_passed` for the pushed revision;
   - the deviations;
   - the review (self or independent, why, and any rejected findings);
   - the browser and bare-board results;
   - any product issues found.
5. Put the PR link in the step's row, then commit and push.
6. Tell the user the PR link and anything they must do. The next step starts in a new session after the merge.

## Working efficiently

- Read a file once. After that, use targeted searches, excerpts and diffs. Keep a short working record: the approved decisions, the paths, the open issues, the checks done and the next action.
- Send long build and test output to logs, and read the summaries and failure excerpts.
- While a build or check runs, keep settled decisions fixed and do useful independent work, such as drafting the next page.
- Put multi-line edits and scripts in files, not in shell heredocs.
- Write the status file once, after review and checks.
- Never cut trying commands, validation or review to save effort. If the step is too large, propose a split.
