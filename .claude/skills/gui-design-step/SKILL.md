---
name: gui-design-step
description: Continue the DriveLink PC GUI design in Figma (phases G0 to G8, and the G7b loop rework) from where the last session stopped. Use when the user says to continue, resume or start the next phase of the GUI design, names a phase id (G3, G4 ...), or asks to change the GUI designs, wireframes or design system. Optional argument is the phase id; without one, take the next step in plans/gui-design-status.md.
---

# Continue the GUI design

The GUI design is design work only, in Figma. One session can run one phase or part of one. Each phase ends with a review by the user before the next one starts. Follow these stages in order, and don't skip the approval points.

## Hard rules

- Design only: no QML or C++. Don't touch `firmware/`, `protocol/` or `docs/protocol/`.
- A firmware or protocol need found while designing becomes a `plans/todo-<topic>.md` file plus a line in the Notes of `plans/overhaul-status.md`, only after the user agrees. Wire changes are the user's decision.
- Decisions are the user's. Ask with options and a recommendation (AskUserQuestion, recommended option first, at most 4 questions per round). Record every answer in `plans/gui-design.md`.
- Commit and push to the design branch named in the status file's handoff note. Open a PR only when the user asks; it goes into `firmware_overhaul`, never `main`.

## 1. Orient

1. Git:
   - `git fetch origin`.
   - Read the "Branch" line of the handoff note in `plans/gui-design-status.md` on `origin/firmware_overhaul` (`git show origin/firmware_overhaul:plans/gui-design-status.md`). If that branch exists on origin and is not merged, switch to it and pull.
   - Otherwise create a new branch from `origin/firmware_overhaul`, say so, and write its name into the handoff note. Never start from an older design branch: it lacks the newer firmware and design records.
   - If the session sits on a branch with unrelated work, stop and tell the user.
2. Read:
   - all of `plans/gui-design-status.md` (state, next step, handoff note, Figma ids, working notes)
   - `plans/gui-design.md`: the Decisions table, the phase row being worked on, and the sections of earlier phases it builds on
   - the parts of `plans/host-architecture.md` ("`drivelink-gui` (Qt/QML)") and `plans/firmware-architecture.md` that the phase shows
3. Pick the work:
   - the argument, if one was given
   - otherwise the "Next step" in the status file
   - If an earlier phase is still `in review`, ask the user first whether it is accepted.
4. Load the Figma tools and skills:
   - Load the Figma MCP tool schemas in one `ToolSearch` call (`use_figma`, `create_session`, `get_libraries`, `search_design_system`, and `generate_diagram` for FigJam work).
   - Load the skills `figma:figma-use` and `figma:figma-generate-design` before building screens; add `figma:figma-generate-library` for design-system work, and `figma:figma-use-figjam` plus `figma:figma-generate-diagram` for FigJam.
   - Call `create_session` with the design file key `49NRHfKe8rOoVlloLLCqLh` (or the FigJam key) and pass its id on every Figma call.
5. Read the current state of the pages you will change with a read-only `use_figma` call (structure, and a `node.screenshot()` of the frames involved). Don't trust ids from memory; use the status file and check they still exist.

## 2. Discuss (approval point)

Before building, ask the user the open questions for this phase, each with 2 to 4 options and a recommendation. Typical topics: layout of the screen, what is shown in which state, interactions, and anything the plans leave open. Use the "Open issues" in `gui-design.md` and the handoff note.

Then post a short scope list (frames or components to make, states covered) and start once the user agrees. Record the answers in the Decisions table of `gui-design.md` right away.

## 3. Build in Figma

- `use_figma` calls run one at a time, never in parallel. Switch page at most once per call. Return the ids of everything created or changed.
- Hi-fi work (G3 on) is built from G2 instances: component sets on the "G2 Design system" page, colours, spacing and radii bound to variables, text styles applied. Don't hardcode colours. If a component is missing, add it to the G2 page first (in the right section, with variants, a QML-mapped description and a row in the QML mapping table), then use it.
- Wireframe changes (G1 page) stay greyscale and use the WF kit.
- Paste `figma-prelude.js` (next to this file) at the top of build calls on the G2 page, and change the page id for other pages.
- Frames are 2560×1440. Every hi-fi screen is also checked at 1920×1080 and in Light mode (set the Color collection's mode on the frame: Dark `24:1`, Light `24:2`).
- Use the sample device from `gui-design.md` for all data, and show the device name and id where a board is identified.
- After each section or screen, take one screenshot with `node.screenshot()` inside `use_figma` and fix what is clipped, overlapping, unreadable or empty. Check against the "Figma working notes" in the status file; add new traps you find there.

## 4. Record

Before finishing, update:
- `plans/gui-design.md`: decisions, the phase row's state (`in review` when built), a section for the phase (what is on the page, frame or component list), and open issues
- `plans/gui-design-status.md`: the state table, "Next step", the handoff note, new Figma ids, new working notes, and a session-log row
- the Cover page phase labels in Figma (G1 `2:25`, G2 `2:30`; find the other rows next to them)
- `plans/host-architecture.md` only if a decision changes the host plan (say so to the user)

## 5. Finish

1. Commit the plan and status changes with a message that says what changed and why, and push the design branch.
2. Tell the user what was built, with the Figma link, what they should review, and what is open. Ask whether the phase is accepted; the next phase starts after that, in this session or a new one with `/gui-design-step`.
