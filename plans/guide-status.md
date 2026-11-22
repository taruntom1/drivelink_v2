# User guide status

The guide shipped in v0.1.1. Its implementation briefs are in
[the guide step archive](archive/guide-steps.md). See
[guide writing rules](../guide/CLAUDE.md) for maintenance and
[host status](host-status.md) for the open hardware checks.

## Current

- **Branches.** Branch from and target `main` for guide fixes.
- **Publishing.** Release tags publish the guide and signed apt repository
  through [GitHub Actions](../packaging/apt/README.md#how-it-is-built-and-published).

## Keeping this file short

Keep product issues and decisions affecting future guide work here. Build
and check instructions live in [development](../docs/development.md).

## Hardware follow-up

The [deferred hardware checks](host-status.md#deferred-hardware-checks)
cover the Pi GPIO UART, discovery from a second computer, the physical IMU
and motors, and floor driving. Update the guide if those runs differ from
the package walkthroughs.

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

## Decision log

Decisions that affect later guide maintenance.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-22 | UG3 | Use the Pi 5 `uart0-pi5` overlay for GPIO 14/15. | The Pi firmware documents this overlay for the reference header. The on-device check remains deferred. | no |
| 2026-11-22 | UG3 | Use the default ROS switch timeout and give `ros2 topic echo` the message types. | The installed Lyrical control CLI misparsed an explicit `--switch-timeout`. Giving the topic type avoids a discovery race observed on Jazzy. The revised commands passed in both distros. | no |
