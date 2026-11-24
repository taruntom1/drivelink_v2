# Declarative CLI configuration status (issue 100)

Tracks the three sequential implementation sessions for
[issue 100](https://github.com/taruntom1/drivelink_v2/issues/100).
The full design, YAML schema, reconciliation rules and step briefs live in
[declarative-config.md](declarative-config.md).
See [host-status.md](host-status.md) for bench board state and open hardware checks.

## Current

- **Next step.** `DC1` (pure C++23 config engine in `libdrivelink`).
- **Integration branch.** `declarative_config` (branched from `main` at `485e104`).
- **PR workflow.** Each step branches from the latest `declarative_config` (for
  example `dc1-config-engine`, `dc2-config-yaml`, `dc3-cli-config`) and opens its
  PR **targeting `declarative_config`**. After `DC1`–`DC3` are merged and verified,
  one promotion PR merges `declarative_config` into `main`.

## Session protocol

Run one step per agent session, sequentially (`DC1` → `DC2` → `DC3`):

1. Read [declarative-config.md](declarative-config.md), this status file, and
   each `CLAUDE.md` along the paths you edit.
2. Start from a clean checkout of the latest `declarative_config` and create a
   step branch from it.
3. Implement only the current step's scope and run its required checks
   ([docs/development.md](../docs/development.md#checks-before-a-pr)) with zero
   warnings.
4. Before opening the PR, update this file in the same PR:
   - Mark the step `in review` (then `merged` once merged, or update at the start
     of the next session) and record its PR link and checks run locally vs CI.
   - Fill in the **Handoff** subsection for the next step with the exact headers,
     types, CMake targets, files added, and any traps discovered.
   - Add a **Decision log** row for any implementation decision that affects a
     later step.
5. Open the PR targeting `declarative_config` and merge once all CI jobs pass.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`.

| Step | Content | Checks | State | PR |
|---|---|---|---|---|
| DC1 | Pure C++23 config engine in `libdrivelink` (`config.hpp`/`.cpp`: `DeviceConfig`, `exportConfig`, `validateConfig`, `planConfig`, `applyConfig` with dependency/limit ordering and `DISCARD_PENDING` rollback) + `host/test/test_config.cpp` unit and vdev tests | `host-win`, `host-linux`, `agent`, `ros` (verify installed `libdrivelink` stays free of JSON/YAML deps) | planned | |
| DC2 | Pinned MIT `rapidyaml` (`host/cmake/Yaml.cmake`), non-ROS `host/config_yaml` static lib (`drivelink_config_yaml`, `COLCON_IGNORE`, `README.md`), third-party notices (`host/notices/` + `gui/notices/`), `host/test/test_config_yaml.cpp` round-trip tests. Depends on `DC1` | `host-win`, `host-linux`, `deb`, `installer` (zero warnings at `-O3` MinGW and GCC 13/15; `gen_notices.py` check) | planned | |
| DC3 | `drivelink-cli config export\|validate\|diff\|apply` (`--patch`, `--reset-defaults`, `--discard-pending`, `--force-device`, `--allow-safe-mode`, `--no-save`, `--reboot [--wait]`), direct + agent integration tests on vdev, CLI/lib READMEs, user guide page, bare-board smoke check. Depends on `DC2` | `quick`, `host-linux`, `agent`, `ros`, `deb`, `installer`, `guide`; existing `host-cli-golden` & `host-cli-replay` untouched; bare-board mock smoke check | planned | |

## Handoff

### Baseline constraints (all steps)

- **No wire or firmware changes.** Core stays v11; `protocol/defs/` and
  `host/test/fixtures/cli-golden.json` do not change.
- **Keep `libdrivelink` dependency-free.** `host/libdrivelink` is built by colcon
  for the ROS 2 packages, and CI verifies installed consumers with JSON discovery
  disabled. YAML/JSON parsing stays outside `libdrivelink` in `host/config_yaml`.
- **Add `COLCON_IGNORE`** to `host/config_yaml/` in `DC2` so colcon never treats
  it as a ROS package.
- **Notices parity.** `DC2` must register `rapidyaml` (MIT) in both
  `host/notices/notices.json` and `gui/notices/notices.json`, add its license
  text, and regenerate both `THIRD_PARTY_NOTICES.txt` files with
  `gen_notices.py --write`.
- **Release `-O3` warnings.** MinGW GCC 13.1 at `-O3` (`installer`) can flag
  warnings that `-O0` (`host-win`) does not; include `rapidyaml` headers as
  `SYSTEM` and check release builds.
- **Bare-board check (`DC3`).** Follow [docs/bench.md](../docs/bench.md): use
  mock modules only (never drive real output pins without asking), verify
  `export`/`diff`/`apply`/`--reboot --wait`, restore the original board layout
  and settings, and log the run in [hardware-log.md](hardware-log.md) (or record
  as deferred in [host-status.md](host-status.md#deferred-hardware-checks) if the
  board is unavailable).

### DC1 → DC2 handoff

*(To be filled in by the `DC1` session: exact `drivelink/config.hpp` types and
field names, value representation for null/gpio/ref/source, error/issue types,
and any edge cases discovered during vdev testing.)*

### DC2 → DC3 handoff

*(To be filled in by the `DC2` session: exact `host/config_yaml` header path,
CMake target name, parse/emit API and error reporting format, pinned `rapidyaml`
version/SHA256, and any MinGW/Linux build notes.)*

## Decision log

Settled pre-implementation decisions (2026-11-25) are recorded in
[declarative-config.md](declarative-config.md#decisions-2026-11-25-user). Add
rows here only for implementation-time decisions that a later step needs.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-25 | Plan | Target `DC1`–`DC3` PRs at integration branch `declarative_config`, then promote to `main`. | Lets three sequential sessions land and pass CI incrementally without exposing half-finished CLI config plumbing on `main`. | yes |
