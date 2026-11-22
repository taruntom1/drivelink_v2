# Repository rules

DriveLink is an ESP32-S3 motion stack with a self-describing device and generic
host. Read [plans/host-status.md](plans/host-status.md) for the hardware
backlog and bench state. For work on the Pages website, see the
[guide rules](guide/CLAUDE.md) and [guide status](plans/guide-status.md).

## Read for the task

Before editing an area, read its scoped rules:

| Area | Rules / reference |
|---|---|
| Firmware | [firmware/CLAUDE.md](firmware/CLAUDE.md); nested rules for components, main and tests |
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
- Before a PR, `python tools/validate.py full` must pass with zero-warning builds.
  Use `quick` while iterating. Update affected API/Kconfig READMEs in the same PR;
  new components need a README. Pure-logic changes need native tests.

- Do not restore retired legacy managers/handlers (`motor_manager`, `imu_bmo_055`,
  `time_sync`) or reuse their diagnostic IDs; history is at `legacy-final`.
- Ask before changing the wire protocol or a safety rule.

## Hardware checks

- Follow the [bench instructions](docs/bench.md) for bare-board checks. Stop
  and report a missing port or unresponsive board.
- Keep deferred checks and pass criteria in [host status](plans/host-status.md).
  Log actual runs in [the hardware log](plans/hardware-log.md).
