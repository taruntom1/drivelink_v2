# User guide status

The working state of the user guide: the steps in [guide-steps.md](guide-steps.md). **Every guide PR updates this file.** A new session reads it first, and the `guide-step` skill does that for you. Hardware state, the bench board and deferred hardware checks stay in [host-status.md](host-status.md), and hardware results go to [hardware-log.md](hardware-log.md).

## Current

- **Planned with the user on 2026-11-22.** The guide comes before the host overhaul's close-out (H6).
- **Next step:** UG3 after UG2 merges.
- **Branches.** Guide steps branch from `origin/host_overhaul` and target `host_overhaul` until H6 merges it into `main`; H6 updates this line.

## Keeping this file short

The rules of [host-status.md](host-status.md) apply ("Keeping this file short"). A fact goes where its subject lives: what a step does is in `guide-steps.md`, how to write a page is in `guide/CLAUDE.md` (from UG1 on), and how to build and check the site is in `docs/development.md`. The handoff is replaced, not appended. A note is deleted by the step that uses it.

## Handoff for UG3

- Read the [UG3 brief](guide-steps.md#ug3-pi-ros-reference-and-troubleshooting), the [writing rules](../guide/CLAUDE.md) and the [site build commands](../docs/development.md#build-and-preview-the-user-guide). Link the Pi and ROS 2 choices on the home page to their journeys.
- Continue the [reference build](../guide/pages/reference/bench.md) from the [PC layout](../guide/pages/pc/layout.md). Use the [remote GUI scenarios](../host/gui/README.md#shell-screenshots) for Pi screenshots and the [host handoff](host-status.md) for ROS container commands and deferred hardware checks.
- Move the Pi and ROS user instructions into the site and regenerate the protocol tables from `protocol/defs/` at build time.
- Before the first guide release tag, follow the [Pages setup](../packaging/apt/README.md#the-signing-key) to switch the source to GitHub Actions and allow deployment tags matching `v*`.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step may split further and add rows here (for example UG2-1 and UG2-2).

| Step | Content | Checks | Review | State | PR |
|---|---|---|---|---|---|
| UG1 | Site, build and `guide` check, theme, Pagefind, web flasher, screenshots in CI, publishing with the apt repository on tags. Home, downloads, apt and flasher pages, plus `guide/CLAUDE.md`. Depends on H4d | Edge desktop and phone, keyboard, print and search. Bare board web flash, esptool and packaged GUI passed. | high, independent CI/publish review passed | merged | [#104](https://github.com/taruntom1/drivelink_v2/pull/104) |
| UG2 | Start here, concepts and glossary, PC bench journey, reference build and wiring diagrams. Depends on UG1 | Edge desktop and phone in both themes, keyboard, print and search. Windows and Ubuntu packages exercised with vdev. | high, self | in review | |
| UG3 | Pi and ROS 2 journeys, generated reference, troubleshooting, About; user text moves out of the repository docs. Depends on UG2 | browser; commands in ROS containers and WSL | high, self | planned | |

## Product issues found

Bugs or gaps in the product found while writing the guide. Each one is told to the user and fixed by a host or HW PR, which deletes its row here.

| Date | Step | Issue | Where the guide works around it |
|---|---|---|---|

## Models and effort

- **One session per step.** It plans and builds the step, and the user approves the plan before any file changes. The session that wrote the plan writes the code.
- **Effort.** High. The status table's "Review" column says when an independent reviewer is required. That reviewer is a separate session or agent that did not write the diff, also at high effort.
- **Current arrangement (user, 2026-11-22).** The steps run in Codex. GPT-6.2 Sol reviews UG1's workflow and publish changes independently. The skill itself names no model, so changing this line is enough.
- **Helpers.** Read-only helpers may search and read. Writing helpers build only parts with a written contract, and the session reviews what they write.

## Decision log

Choices the plans don't cover or that change them, and that a later step must respect. Anything that touches a decision in [guide-steps.md](guide-steps.md), the wire, a safety rule or the release process is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-22 | UG2 | Use one Pololu G2 18v17 per motor in the reference build. | The released firmware uses 25 kHz PWM. The MDD10A is rated to 20 kHz. | yes (`guide-steps.md`) |
