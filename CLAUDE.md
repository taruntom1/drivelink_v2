# CLAUDE.md

This file is the repo map and holds the rules that cross areas. Each area has its own `CLAUDE.md` with the rules for working in it.

## What this is

DriveLink v2 is a robot motion stack: ESP32-S3 firmware (ESP-IDF 5.5.2, C++) that drives motors, reads encoders and sensors and runs the control loops, plus host software that talks to the board over a binary UART protocol. The host side is being planned and is not built yet ([plans/host-architecture.md](plans/host-architecture.md)).

## Repo map

| Path | What | Rules |
|---|---|---|
| `firmware/` | The ESP-IDF project: `main/`, `components/`, `test/host` (pure-logic unit tests), `test/uart_comm` (hardware-in-the-loop tests) | [firmware/CLAUDE.md](firmware/CLAUDE.md), and [firmware/components/CLAUDE.md](firmware/components/CLAUDE.md) for components |
| `protocol/` | The wire library shared by firmware and host: framing, CRC, frame scanner, `wire_codec`. Pure C++20 | [protocol/CLAUDE.md](protocol/CLAUDE.md) |
| `docs/protocol/` | The wire spec and the diag code table | Updated in the same commit as any wire change |
| `tools/` | Generators and scripts (`gen_diag_codes.py`) | |
| `plans/` | The overhaul plans and `overhaul-status.md` | |
| `.github/workflows/` | CI | |

`host/` joins the repo in a later overhaul step, with its own `CLAUDE.md`.

## Cross-cutting rules

- **Host and firmware change together.** There is no backward compatibility on the wire. Any protocol change updates the firmware, the host code that speaks it, and `docs/protocol/` in one change.
- **Definition of done** is per area. For the firmware it is the list in `firmware/CLAUDE.md` ("Definition of done"): zero-warning builds, current generated files, passing host tests, docs in the same commit. CI checks it.
- **Paths.** Paths in `firmware/CLAUDE.md` are relative to the repository root, and firmware commands run from the root with `-C firmware`.

## Firmware overhaul in progress (temporary, removed in F10)

The firmware is being rebuilt to [plans/firmware-architecture.md](plans/firmware-architecture.md), and the host follows ([plans/host-architecture.md](plans/host-architecture.md)). Where this section conflicts with the rest of this file or with `firmware/CLAUDE.md`, this section wins. `firmware/CLAUDE.md` describes the code as it is now, and each step updates it as it changes the code.

- **Start here.** Read [plans/overhaul-status.md](plans/overhaul-status.md) first: the next step, the handoff note and the decision log. Start a step with `/overhaul-step`.
- **One session per step.**
  - Branch from `origin/firmware_overhaul`, and open one PR into `firmware_overhaul` (`gh pr create --base firmware_overhaul`).
  - Never target `main`. It is frozen except for critical fixes.
- **Every PR updates the status file:** the step's state, its PR link, any decisions, and the handoff note for the next step.
- **Plan changes.**
  - Small internal choices: decide, and log them in the decision log.
  - Anything that changes the wire protocol, a safety rule or a later phase: stop and ask the user. Then update the plan in the same PR.
- **Docs.**
  - The component READMEs and `review/` were removed. `docs/protocol/` is the wire spec, updated in the same PR as any wire change.
  - New components get a short README (purpose, API, rules) when they land.
  - There are no `review/` statuses or `TODO.md` to update.
- **Legacy code.**
  - `motor_manager`, `imu_bmo_055` and `time_sync` keep working until their phase ports them.
  - Don't improve code that a later phase deletes. Fix it only if it blocks the current step.
  - Their wire format is defined by their code and host tests. The deleted READMEs are in git history (`git show 7a4d693:components/<name>/README.md`).
- **Hardware.** Sessions don't flash the board. A step that needs a hardware check stops before its PR and gives the user a short test script: what to flash, what to run, what to look for.
