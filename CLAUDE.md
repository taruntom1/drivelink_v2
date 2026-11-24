# Repository rules

DriveLink is an ESP32-S3 motion stack with a self-describing device and generic
host. Read [host status](plans/host-status.md) when the task depends on board
state, deferred hardware checks, or the current host milestone. For work on the Pages website, see the
[guide rules](guide/CLAUDE.md) and [guide status](plans/guide-status.md).

## Read for the task

Before editing a file, read each `CLAUDE.md` along the directory path from
the repository root through the file's containing directory.

| Area | Rules / reference |
|---|---|
| Firmware | [firmware/CLAUDE.md](firmware/CLAUDE.md) |
| Host | [host/CLAUDE.md](host/CLAUDE.md), [build and usage](host/README.md) |
| Shared wire library | [protocol/CLAUDE.md](protocol/CLAUDE.md) |
| Definitions / generator | [docs/protocol/definitions.md](docs/protocol/definitions.md) |
| New module type | [docs/adding-a-module.md](docs/adding-a-module.md) |
| Build, validation, bench commands | [docs/development.md](docs/development.md) |

## Cross-area constraints

- No wire backward compatibility. Wire changes bump the affected version and
  update firmware, host and `docs/protocol/` in the same change.
- Edit `protocol/defs/` or the generator, run `python tools/gen/gen.py`, and
  commit generated headers/docs. Never hand-edit generated outputs.
- One version for the repo: the root `VERSION` (firmware, host, `package.xml`,
  installer, `.deb`); `tools/version.py --check` enforces it.
- Before a PR, the [checks for the areas it touches](docs/development.md#checks-before-a-pr)
  must pass with zero-warning builds, and the bare-board checks if firmware
  behavior on the device changes. CI runs every check: all its jobs must pass
  before a PR is merged. Name in the PR what ran locally and what is left to CI.
  While iterating, use `--only` for the affected checks (for example,
  `python tools/validate.py --only guide`) or `quick` for broader feedback.
  Update affected API/Kconfig READMEs in the same PR;
  new components need a README. Pure-logic changes need native tests.

- Do not restore retired legacy managers/handlers (`motor_manager`, `imu_bmo_055`,
  `time_sync`) or reuse their diagnostic IDs; history is at `legacy-final`.
- Ask before changing the wire protocol or a safety rule.

## Hardware checks

- Follow the [bench instructions](docs/bench.md) for bare-board checks. A PR runs
  the ones for the affected area when firmware behavior on the device changes
  (not for comments, docs or a new descriptor hash alone). Ask before driving
  output pins, and restore the configuration. If the
  port is missing or the board is unresponsive, stop hardware-dependent work,
  report the blocker and pending checks, and continue independent work. A
  missing board, motor or sensor does not block a PR: the check becomes deferred.
- Keep deferred checks and pass criteria in [host status](plans/host-status.md).
  Log actual runs in [the hardware log](plans/hardware-log.md).
