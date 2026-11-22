# User guide status

The working state of the user guide: the steps in [guide-steps.md](guide-steps.md). **Every guide PR updates this file.** A new session reads it first, and the `guide-step` skill does that for you. Hardware state, the bench board and deferred hardware checks stay in [host-status.md](host-status.md), and hardware results go to [hardware-log.md](hardware-log.md).

## Current

- **Planned with the user on 2026-11-22.** The guide comes before the host overhaul's close-out (H6).
- **Next step:** H6 closes out the host overhaul.
- **Branches.** Guide steps branch from `origin/host_overhaul` and target `host_overhaul` until H6 merges it into `main`; H6 updates this line.

## Keeping this file short

The rules of [host-status.md](host-status.md) apply ("Keeping this file short"). A fact goes where its subject lives: what a step does is in `guide-steps.md`, how to write a page is in `guide/CLAUDE.md` (from UG1 on), and how to build and check the site is in `docs/development.md`. The handoff is replaced, not appended. A note is deleted by the step that uses it.

## Handoff for H6

- Read the [host handoff](host-status.md#handoff-for-h6--hw) and the [H6 brief](host-steps.md#h6-close-out). Update the guide's branch policy when the overhaul merges into `main`.
- The [deferred hardware checks](host-status.md#deferred-hardware-checks) cover the Pi GPIO UART, discovery from a second computer, the physical IMU and motors, and floor driving. Update the guide if the real runs differ from the package walkthroughs.
- Release tags publish the guide and signed apt repository through [GitHub Actions](../packaging/apt/README.md#how-it-is-built-and-published).

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step may split further and add rows here (for example UG2-1 and UG2-2).

| Step | Content | Checks | Review | State | PR |
|---|---|---|---|---|---|
| UG1 | Site, build and `guide` check, theme, Pagefind, web flasher, screenshots in CI, publishing with the apt repository on tags. Home, downloads, apt and flasher pages, plus `guide/CLAUDE.md`. Depends on H4d | Edge desktop and phone, keyboard, print and search. Bare board web flash, esptool and packaged GUI passed. | high, independent CI/publish review passed | merged | [#104](https://github.com/taruntom1/drivelink_v2/pull/104) |
| UG2 | Start here, concepts and glossary, PC bench journey, reference build and wiring diagrams. Depends on UG1 | Edge desktop and phone in both themes, keyboard, print and search. Windows and Ubuntu packages exercised with vdev. | high, self | merged | [#106](https://github.com/taruntom1/drivelink_v2/pull/106) |
| UG3 | Pi and ROS 2 journeys, generated reference, troubleshooting, About; user text moves out of the repository docs. Depends on UG2 | Full gate, release-mode site and apt checks, packaged GUI with vdev in WSL, and installed ROS commands with vdev on Lyrical and Jazzy. Browser review skipped at the user's request. | high, self | merged | [#107](https://github.com/taruntom1/drivelink_v2/pull/107) |

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
| 2026-11-22 | UG3 | Use the Pi 5 `uart0-pi5` overlay for GPIO 14/15. | The Pi firmware documents this overlay for the reference header. The on-device check remains deferred. | no |
| 2026-11-22 | UG3 | Use the default ROS switch timeout and give `ros2 topic echo` the message types. | The installed Lyrical control CLI misparsed an explicit `--switch-timeout`. Giving the topic type avoids a discovery race observed on Jazzy. The revised commands passed in both distros. | no |
