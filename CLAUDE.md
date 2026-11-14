# Repository rules

DriveLink is an ESP32-S3 motion stack with a self-describing device and generic
host. Read [plans/host-status.md](plans/host-status.md) first for current
work, handoff and hardware state.

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
- Before a PR, `python tools/validate.py full` must pass with zero-warning builds.
  Use `quick` while iterating. Update affected API/Kconfig READMEs in the same PR;
  new components need a README. Pure-logic changes need native tests.

## Host overhaul in progress

This temporary policy wins over conflicting area rules until `host_overhaul`
merges into `main`. Architecture targets and decisions live in `plans/`.

- Start each step with `/overhaul-step`; one session and one PR per step.
  Branch from `origin/host_overhaul`, target `host_overhaul`, never frozen `main`.
  HW firmware fixes use the same base. Every PR updates status, decisions and handoff.
- Log small internal choices in the status decision log. Ask the user before
  changing the wire protocol, a safety rule or a later step; update the plan too.
- The session plans, builds and reviews its step. Model/effort and independent
  review criteria are in the status file's "Models and effort" and the skill.
- Do not restore retired legacy managers/handlers (`motor_manager`, `imu_bmo_055`,
  `time_sync`) or reuse their diagnostic IDs; history is at `legacy-final`.
- After review, run bare-board checks directly; stop/report a missing port or
  unresponsive board. For motors/IMU/robot, write and dry-run scripts and list
  deferred checks in the status file. For Pi/PC-only checks, supply the user's
  script and wait in `waiting on hardware`. Real motor/IMU behavior is unverified
  until those checks pass; use the status file for current results.
