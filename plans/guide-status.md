# User guide status

The working state of the user guide: the steps in [guide-steps.md](guide-steps.md). **Every guide PR updates this file.** A new session reads it first, and the `guide-step` skill does that for you. Hardware state, the bench board and deferred hardware checks stay in [host-status.md](host-status.md), and hardware results go to [hardware-log.md](hardware-log.md).

## Current

- **Planned with the user on 2026-11-22.** The guide comes before the host overhaul's close-out (H6).
- **Next step:** UG2 after UG1 merges.
- **Branches.** Guide steps branch from `origin/host_overhaul` and target `host_overhaul` until H6 merges it into `main`; H6 updates this line.

## Keeping this file short

The rules of [host-status.md](host-status.md) apply ("Keeping this file short"). A fact goes where its subject lives: what a step does is in `guide-steps.md`, how to write a page is in `guide/CLAUDE.md` (from UG1 on), and how to build and check the site is in `docs/development.md`. The handoff is replaced, not appended. A note is deleted by the step that uses it.

## Handoff for UG2

- Read the [UG2 brief](guide-steps.md#ug2-start-here-concepts-and-the-pc-bench-journey), [writing rules](../guide/CLAUDE.md) and [site build commands](../docs/development.md#build-and-preview-the-user-guide). Add pages to `guide/navigation.json` and wire the PC choice on the home page to the full bench journey.
- Use `guide/screenshots.json` and the [GUI scenarios](../host/gui/README.md#shell-screenshots). Try the desktop steps against vdev using the packaged app. UG1 installed the Windows v0.1.0 installer in Windows Sandbox and exercised the signed preview in both Ubuntu containers.
- Confirm the reference parts list during UG2 planning. The first flash already has browser and esptool instructions. The [bench record](hardware-log.md) has the v0.1.0 flash results.
- Before the first guide release tag, follow the [Pages setup](../packaging/apt/README.md#the-signing-key) to switch the source to GitHub Actions and allow deployment tags matching `v*`.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step may split further and add rows here (for example UG2-1 and UG2-2).

| Step | Content | Checks | Review | State | PR |
|---|---|---|---|---|---|
| UG1 | Site, build and `guide` check, theme, Pagefind, web flasher, screenshots in CI, publishing with the apt repository on tags. Home, downloads, apt and flasher pages, plus `guide/CLAUDE.md`. Depends on H4d | Edge desktop and phone, keyboard, print and search. Bare board web flash, esptool and packaged GUI passed. | high, independent CI/publish review passed | in review | [#104](https://github.com/taruntom1/drivelink_v2/pull/104) |
| UG2 | Start here, concepts and glossary, PC bench journey, reference build and wiring diagrams. Depends on UG1 | browser; GUI steps against vdev | high, self | planned | |
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
| 2026-11-22 | UG1 | The user approved an Actions deployment from the verified release artifact, with the same tree mirrored to `gh-pages`. Each publish attempt names its own artifact and passes the name to the deploy job. | Preserve apt URLs and make release retries work with the artifact from the successful publish attempt. | yes (`guide-steps.md`) |
