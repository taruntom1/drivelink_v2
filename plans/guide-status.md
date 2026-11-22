# User guide status

The working state of the user guide: the steps in [guide-steps.md](guide-steps.md). **Every guide PR updates this file.** A new session reads it first, and the `guide-step` skill does that for you. Hardware state, the bench board and deferred hardware checks stay in [host-status.md](host-status.md), and hardware results go to [hardware-log.md](hardware-log.md).

## Current

- **Planned with the user on 2026-11-22.** The guide comes before the host overhaul's close-out (H6).
- **Next step:** UG1.
- **Branches.** Guide steps branch from `origin/host_overhaul` and target `host_overhaul` until H6 merges it into `main`; H6 updates this line.

## Keeping this file short

The rules of [host-status.md](host-status.md) apply ("Keeping this file short"). A fact goes where its subject lives: what a step does is in `guide-steps.md`, how to write a page is in `guide/CLAUDE.md` (from UG1 on), and how to build and check the site is in `docs/development.md`. The handoff is replaced, not appended. A note is deleted by the step that uses it.

## Handoff for UG1

- Read the [decisions](guide-steps.md#decisions) and the [UG1 brief](guide-steps.md#ug1-site-build-and-publishing), then [apt packaging](../packaging/apt/README.md) and the `apt-repo`, `apt-publish` and `release` jobs in `.github/workflows/ci.yml`.
- The guide takes over the Pages root from `packaging/apt/index.html`. The apt paths must not change, because v0.1.0 users and the H3d and H4d deferred rows use them.
- **Inputs to reuse:**
  - `host/gui/theme/tokens.json` and `gen_theme.py` for the theme;
  - `host/gui/resources/` for the fonts, icons and their `SOURCES.md`;
  - the GUI screenshot scenarios (`host/gui/README.md`, "Shell screenshots"; `remote_scenarios.py` for the Pi pages in UG3);
  - the firmware package's `flash_args.json` and merged image (`tools/package_fw.py`);
  - `protocol/defs/*.yaml` for UG3's generated reference.
- **Traps:**
  - Material for MkDocs reached end of life on 2026-11-05. That is one reason the guide has its own build.
  - Web Serial works only in Chromium browsers on a secure origin. Pages serves HTTPS, but a local `serve` must use `localhost`.
  - The host traps in [host-status.md](host-status.md) (WSL, Docker, the ROS containers) apply whenever a guide command is tried.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step may split further and add rows here (for example UG2-1 and UG2-2).

| Step | Content | Checks | Review | State | PR |
|---|---|---|---|---|---|
| UG1 | Site, build and `guide` check, theme, Pagefind, web flasher, screenshots in CI, publishing with the apt repository on tags; home, downloads, flasher pages, `guide/CLAUDE.md`. Depends on H4d | browser; bare board: web flash (ask first) | high; independent for CI and publish | planned | |
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
| 2026-11-22 | all | The user planned the guide: a hand-built site (Markdown, our own Python build, Pagefind) at the Pages root beside unchanged apt paths, published only by tags. Its theme comes from the GUI tokens. It has a web flasher and screenshots rendered in CI. It covers the PC, Pi and ROS journeys and leaves out the CLI. Unverified hardware pages are written as finished, and the guide owns the user text. The work is split into UG1 to UG3 before H6. | Users have only the released artifacts and no ROS or embedded background, and each fact has one source. | yes (`guide-steps.md`) |
