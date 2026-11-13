# CLAUDE.md

This file is the repo map and holds the rules that cross areas. Each area has its own `CLAUDE.md` with the rules for working in it.

## What this is

DriveLink v2 is a robot motion stack: ESP32-S3 firmware (ESP-IDF 5.5.2, C++) and generic host software over binary UART. The core has configurable loops, groups, safety, sources, motor modules and the F6 `imu.bno055` module with persistent calibration, and F7a capture into PSRAM, over core wire v6. Only the legacy `time_sync` manager remains, until F8. The host follows [plans/host-architecture.md](plans/host-architecture.md).

## Repo map

| Path | What | Rules |
|---|---|---|
| `firmware/` | The ESP-IDF project: `main/`, `components/`, `test/native` (native unit tests: `protocol/`, the core, pure logic), `test/uart_comm` (hardware-in-the-loop tests) | [firmware/CLAUDE.md](firmware/CLAUDE.md), and [firmware/components/CLAUDE.md](firmware/components/CLAUDE.md) for components |
| `protocol/` | The wire library shared by firmware and host: framing, CRC, frame scanner, `wire_codec`, and the generated registry, diag codes and descriptor format. Pure C++20 | [protocol/CLAUDE.md](protocol/CLAUDE.md) |
| `protocol/defs/` | The definitions (YAML): `registry.yaml` (MSG_TYPEs, stream channel ranges), `diag_codes.yaml`, `system.yaml` (the system items, type `0x00`) and, from F3b, one file per module type. The source of everything under `protocol/include/drivelink/protocol/gen/` | [docs/protocol/definitions.md](docs/protocol/definitions.md) |
| `docs/protocol/` | The wire spec, the generated registry and diag code tables, and the definitions guide | Updated in the same commit as any wire change |
| `tools/gen/` | The generator: `python tools/gen/gen.py [--check]` turns `protocol/defs/` into the generated headers and docs. Its tests: `python -m unittest discover -s tools/gen/tests` | [docs/protocol/definitions.md](docs/protocol/definitions.md) |
| `tools/validate.py` | The list of validation checks, run locally (`quick`, `full`) and by CI (`--only`). Never touches hardware | [firmware/test/CLAUDE.md](firmware/test/CLAUDE.md) ("Validation") |
| `plans/` | The overhaul plans and `overhaul-status.md` | |
| `.github/workflows/` | CI | |

`host/` contains the first `libdrivelink`, `cli`, `vdev`, `superbuild` and host tests. Its rules are in [host/CLAUDE.md](host/CLAUDE.md), build and usage in [host/README.md](host/README.md).

## Cross-cutting rules

- **Host and firmware change together.** There is no backward compatibility on the wire. Any protocol change updates the firmware, the host code that speaks it, and `docs/protocol/` in one change.
- **Definition of done** is per area. For the firmware it is the list in `firmware/CLAUDE.md` ("Definition of done"): zero-warning builds, current generated files, passing native tests, docs in the same commit. `python tools/validate.py full` checks the builds and tests locally, and CI runs the same checks through the same script.
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
  - `time_sync` keeps working until F8 ports it (`motor_manager` was removed in F5b, `imu_bmo_055` in F6).
  - Don't improve code that a later phase deletes. Fix it only if it blocks the current step.
  - Their wire format is defined by their code and native tests. The deleted READMEs are in git history (`git show 7a4d693:components/<name>/README.md`).
- **Roles.** The session plans, builds and reviews the step itself (Opus 5.5, high). It spawns an independent review agent (Opus, at the step's review effort) only when the step needs one: wire, safety, persistence or concurrency changes it can't fully cover itself, or a large diff. `/overhaul-step` has the details.
- **Hardware.**
  - Checks that run on the bare bench board (nothing attached) if necessary: the session builds, flashes and runs them itself, after the review. It stops and tells the user if a port is missing or the board doesn't respond.
  - Checks that need motors, an IMU or the robot are deferred: the step writes and dry-runs their script and lists it in "Deferred hardware checks" in the status file. They run in the HW step after F10, or as soon as the hardware arrives.
