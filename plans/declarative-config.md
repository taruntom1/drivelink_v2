# Declarative CLI configuration (issue 100)

Plan for [issue 100](https://github.com/taruntom1/drivelink_v2/issues/100): declare a
device's identity, system settings, loops, module instances, wiring, sources and
parameters in a YAML (or JSON) file, and reconcile, diff, validate or export it
from `drivelink-cli`. No wire protocol or firmware changes are needed.

Track step progress, PRs, and session handoffs in
[declarative-config-status.md](declarative-config-status.md).

## Why it fits the current design

DriveLink's firmware is already self-describing (`GET_TYPES`, `GET_ITEMS`, `GET_ENUM`,
`LIST_INSTANCES`, `GET` / `GET_PENDING`) and `libdrivelink`'s `DeviceModel` already
builds the complete typed tree of types, loops, instances, params, defaults, ranges,
enum names, `gpio` pins, `ref` wiring and `source` links from the device.

Rather than adding motor-specific syntax (`motors:`) that would break the generic-host
rule and require CLI updates for every new module type, the schema maps 1:1 onto the
device's discovered descriptors. Any current or future module (`actuator.pwm_dir`,
`feedback.quadrature`, `controller.cascade_pid`, `imu.bno055`, `fusion.mock`, mocks,
and future `link.*` settings from [runtime-settings.md](runtime-settings.md)) works
with zero module-specific host code and round-trips cleanly between `export` and `apply`.

## Decisions (2026-11-25, user)

| Topic | Decision |
|---|---|
| Schema | Purely generic, descriptor-driven (`version`, `device`, `system`, `loops`, `instances`). No module-specific shorthand or host module table. |
| File format | YAML (with JSON accepted as a YAML 1.2 subset). |
| YAML dependency | Pinned MIT `rapidyaml` in a non-ROS static host library (`host/config_yaml`, with `COLCON_IGNORE`), so `libdrivelink` stays pure C++23 with zero YAML/JSON deps for colcon/ROS builds. |
| Layering | Pure C++23 config model, validator, planner/diff, applier and exporter live in `libdrivelink` so `drivelink-gui` can reuse them later; YAML/JSON I/O lives in `host/config_yaml` and command wiring/formatting in `drivelink-cli`. |
| Subcommands | `config export`, `config diff`, `config validate`, `config apply`. |
| Dotted param names | Canonical flat descriptor names on `export` (`vel.kp`, `limits.loops`); `apply`/`diff`/`validate` accept both flat keys and nested maps (`vel: { kp: 0.4 }`). |
| Reconciliation | Authoritative on loops and instances by default (removes unlisted ones, requires full layout sections); `--patch` allows partial files (e.g. tuning-only or system-only overlays) without pruning unlisted entries. |
| Omitted params | Preserved on existing instances by default (protects tuned gains and IMU calibration); `--reset-defaults` resets omitted writable params to descriptor defaults. |
| Numeric range checks | Strict client-side validation against descriptor `[min, max]`; reject out-of-range values (exit 2) instead of letting the firmware silently clamp them. |
| Export defaults | Concise by default: emit non-default writable params plus all `gpio`, `ref`, `source` wiring and loop `rate_hz`/`realtime`; omit `device.id` for portability. Flags: `--all-params`, `--pending`, `--include-device-id`. |
| Safety guards & rollback | Refuse if Armed, in safe mode (`--allow-safe-mode` to override), or if unexpected pending edits exist (`--discard-pending` to override); refuse on `device.id` mismatch (`--force-device` to override). Automatically call `DISCARD_PENDING` if staging fails partway through. |
| Commit & reboot | `config apply` runs `SAVE` automatically when changes were made (opt-out with `--no-save`). Never reboots unless `--reboot` is passed; `--reboot` sends `REBOOT` and exits (matching `drivelink-cli reboot`), with optional `--wait` to reconnect/poll until the device is back and verify all instances start `Running`. |

## File format (`version: 1`)

```yaml
version: 1

device:
  name: differential_drive
  # id: 1cdbd446ff38  # optional 12-hex factory MAC guard

system:
  watchdog_ms: 200
  status.rate_hz: 10
  limits.loops: 4
  limits.instances: 16

loops:
  control:
    rate_hz: 1000
    realtime: true
    core: core_1
    priority: 20
  sensor:
    rate_hz: 100
    realtime: false

instances:
  left_motor:
    type: actuator.pwm_dir
    loop: control
    params:
      pwm_pin: 4
      dir_pin: 5
      pwm_freq_hz: 25000

  left_enc:
    type: feedback.quadrature
    loop: control
    params:
      pin_a: 6
      pin_b: 7
      counts_per_rev: 1200
      gear_ratio: 10

  left_ctrl:
    type: controller.cascade_pid
    loop: control
    params:
      actuator: left_motor       # ref -> instance label (null = unwired)
      feedback: left_enc
      mode: velocity
      vel.kp: 0.4
      vel.ki: 1.2

  imu:
    type: imu.bno055
    loop: sensor
    params:
      sda_pin: 8
      scl_pin: 9
```

### Schema rules

- **Top-level keys:** `version` (required, integer `1`), `device` (optional), `system` (optional), `loops` (required in full mode, optional in `--patch`), `instances` (required in full mode — may be `{}` — optional in `--patch`). Unknown top-level keys are rejected (exit 2).
- **`device`:**
  - `name`: string (0..16 UTF-8 bytes, no C0/C1 control chars; empty `""` clears the name; omitted leaves it unchanged).
  - `id`: optional 12 lowercase hex digits (factory MAC). If present, `apply`/`diff` refuse a device with a different id unless `--force-device` is given.
- **`system`:** map of writable system param names (type 0) to typed values. Nested maps are flattened with `.` (`limits: { loops: 4 }` ≡ `limits.loops: 4`). Rejects duplicate flattened keys, unknown names, read-only items, and non-param items.
- **`loops`:** map keyed by loop label (1..16 valid non-NUL UTF-8 bytes). Values are maps of writable `loop` (type `0x05`) params (`rate_hz`, `realtime`, `core`, `priority`, `stack_bytes`), supporting flat or nested keys. Empty map `{}` is valid (zero loops).
- **`instances`:** map keyed by instance label (1..16 valid non-NUL UTF-8 bytes, unique across all loops and instances in the file). Each entry has:
  - `type`: required discovered module type name (e.g. `actuator.pwm_dir`; `system` and `loop` are rejected here).
  - `loop`: required label of a loop declared in `loops` (or existing on the device in `--patch` mode).
  - `params`: optional map of writable param names to values (flat or nested).
- **Value encoding (descriptor-driven):**
  - `bool`: YAML boolean (`true` / `false`).
  - `u8`, `u16`, `u32`, `i32`: exact integer within the C++ type range and descriptor `[min, max]` (rejects floats, strings, overflow, out-of-range).
  - `f32`: finite number within descriptor `[min, max]` (rejects NaN, Inf, out-of-range).
  - `enum`: discovered enum entry name string (exact match against `GET_ENUM`).
  - `gpio` (`ItemFlag::Gpio`): integer pin number or `null` (`0xFF`, no pin).
  - `ref` (wiring param, `ItemDescriptor::ref` non-empty): target instance label string or `null` (`0`, unwired). Client-side validation checks the target exists, matches the required category (`defs::inCategory`), and runs in the same loop.
  - `source` (`ItemFlag::Source`): `<instance_label>.<signal_or_input_name>` string or `null` (`0`, disconnected). Client-side validation checks the target instance and item exist, item kind is `signal` or `input`, and value type matches the source's value type.

## Reconciliation, ordering, and safety

### Instance matching
- Matching between the file and the connected device is by **label** (consistent with `drivelink_hardware`).
- If the device already contains duplicate labels among loops or among instances, `diff`/`apply` fail loudly (exit 2) naming the duplicate labels so nothing is mis-matched.
- An existing instance whose **type** or **loop** changed cannot be mutated in place on the wire; the planner models it as a **replace** (remove old + add new, transferring preserved params only when type is unchanged and `--reset-defaults` is not set, or re-applying declared params).
- Changing a loop's `realtime` flag is a normal `reboot_required` param write on the existing loop instance (no remove/re-add needed).

### Preflight validation (before any wire write)
All checks run against the loaded `DeviceSnapshot` before sending a single mutation:
1. Schema, types, enum names, UTF-8 label/name lengths, and descriptor `[min, max]` bounds.
2. Module `loop_mask` compatibility (`realtime: required | forbidden | any`) against the target loop's effective `realtime` setting.
3. Wiring `ref` category and same-loop checks, plus cycle detection on both dependency (`ref`) and tick-order (`ref_order`) graphs, plus same-loop `source` cycle detection.
4. GPIO uniqueness across all desired instances (no pin claimed twice).
5. Pending limits sanity (`loops.size() <= effective limits.loops`, `instances.size() <= effective limits.instances`), noting that RAM and board-specific GPIO policy are still authoritatively checked by the firmware during staging.
6. Device state guards on `apply`:
   - Refuse if `arm_state != Disarmed` (exit 5 / 2 — never auto-disarm).
   - Refuse if `safe_mode` unless `--allow-safe-mode` (exit 5).
   - Refuse if `pending_reboot` or pending layout/system diffs exist unless `--discard-pending` (exit 5).
   - Refuse if `device.id` is specified and differs from `info.device_id` unless `--force-device` (exit 2).

### Deterministic execution order
The planner produces a typed, inspectable `ConfigPlan` (used by both `diff` and `apply`) split into ordered steps so firmware constraints hold at every intermediate op:

1. **Optional discard:** if `--discard-pending` and prior pending edits exist, call `DISCARD_PENDING`.
2. **Raise `limits.*` (reboot-required system params that increase):** staged first so subsequent `ADD_INSTANCE` calls see the higher pending limits in the same session.
3. **Clear pins / break references & remove instances:**
   - Unwire refs/sources on kept instances that point to removed/replaced instances (so `REMOVE_INSTANCE` is never blocked by `Referenced`).
   - Remove deleted/replaced module instances in reverse dependency order (dependents before dependencies; e.g., controllers before actuators/feedback, source consumers before producers).
   - Remove deleted loops after all their module instances are removed.
   - Free pins on kept instances whose pin is being reassigned to another instance (set to `0xFF` first if a pin swap occurs between two kept instances).
4. **Add loops & stage loop params:** `ADD_INSTANCE` for new loops (recording the firmware-allocated loop IDs), then stage `reboot_required` loop params (`realtime`, `core`, `priority`, `stack_bytes`) and `rate_hz` for new loops.
5. **Add module instances:** `ADD_INSTANCE` for new/replaced module instances using resolved loop IDs (recording allocated instance IDs).
6. **Stage `reboot_required` module params:** write pins, `ref` wiring (encoding `wiringRef(type, inst)` with newly allocated IDs), `source` links (encoding `sourceRef(type, inst, item)` with newly allocated IDs), and other `reboot_required` params in pending-domain batches (at most 64 entries per batch).
7. **Lower `limits.*` & other `reboot_required` system params:** staged after removals so lowered limits never collide with removed instances.
8. **Apply live params & device name:**
   - Live system params (config-task domain batch) and live loop `rate_hz` on existing loops.
   - Live module params on existing instances, grouped by actual running loop ID (per firmware's single-loop atomic batch rule; at most 64 per batch). *(Live params on newly added pending-only instances are staged in step 6 in the pending domain.)*
   - `SET_NAME` if `device.name` changed.
9. **Save:** unless `--no-save`, call `SAVE` if any mutation was performed (or if `nvs_dirty` became set).
10. **Reboot (optional):** only if `--reboot` was passed and reboot-required changes were staged (or `--reboot-always`? Keep simple: `--reboot` reboots if reboot-required changes exist and reports `rebooted: false` with a clear note if only live changes were applied). With `--wait`, poll/reconnect (respecting `--timeout-ms` / `--wait-timeout-ms`), verify `device_id` matches, and verify every configured instance is `Running` (non-zero exit if any instance is `Failed`, `Invalid`, or `DependencyMissing`).

### Failure & rollback behavior
- All staged layout and `reboot_required` operations (steps 2–7) run **before** live parameter writes (step 8).
- If any operation in steps 2–7 fails (for example `NotEnoughMemory`, bad GPIO policy, or capacity refusal), `apply` automatically calls `DISCARD_PENDING` (when no pre-existing pending state was kept) and reports the original error, `result`, `detail`, and rollback status.
- A write timeout (`outcome_unknown`) is reported faithfully without blind retry, matching [host/CLAUDE.md](../host/CLAUDE.md).
- Through `drivelink-agent`: exclusive ops (`add`, `remove`, `discard`, `setName`, `save`, `reboot`) use the existing `--device` / `--token` / `--lease` contract; if another client holds the lease and `--lease` is omitted, the agent refuses cleanly (`Busy/LeaseRequired`, exit 5).

## CLI surface

```sh
drivelink-cli [GLOBAL_OPTS] config export [FILE] [--pending] [--all-params] [--include-device-id]
drivelink-cli [GLOBAL_OPTS] config validate FILE [--patch]
drivelink-cli [GLOBAL_OPTS] config diff FILE [--patch] [--reset-defaults] [--force-device]
drivelink-cli [GLOBAL_OPTS] config apply FILE [--patch] [--reset-defaults] [--discard-pending]
                                              [--force-device] [--allow-safe-mode]
                                              [--no-save] [--reboot [--wait]]
```

- `config export [FILE]`: writes canonical YAML to `FILE` (or stdout if omitted; with `--json`, emits `{"ok":true,"data":{"yaml":...,"config":...}}`).
- `config validate FILE`: connects to the device, loads descriptors, and validates the file without mutating anything. Prints errors with YAML path context (e.g. `instances.left_ctrl.params.vel.kp: value 5000 exceeds max 1000`) and exits 0 (valid) or 2 (invalid).
- `config diff FILE`: computes and prints the planned changes (device name, system params, loops added/removed/changed, instances added/removed/replaced/changed, whether `reboot_required` is true). Exit 0 even when diffs exist (with `--json` reporting `has_changes`, `reboot_required`, and structured change lists).
- `config apply FILE`: prints the summary of applied changes, whether `saved`, `pending_reboot`, and `rebooted` occurred, and reminds the user to run `reboot` if `pending_reboot` is true and `--reboot` was not passed (or notes a link trial if a future `link.*` param was changed).

## Implementation steps

Three PRs (`DC1`–`DC3`), one per step, in order, each targeting the
`declarative_config` integration branch (tracked in
[declarative-config-status.md](declarative-config-status.md)).

### Step 1 (`DC1`): Pure C++23 config engine in `libdrivelink`

- Add public headers `host/libdrivelink/include/drivelink/config.hpp` and implementation `host/libdrivelink/src/config.cpp` (no YAML/JSON deps; pure C++23).
- Types: `DeviceConfig`, `LoopConfig`, `InstanceConfig`, `ConfigValue` (typed scalar/enum/null/ref-label/source-label), `ConfigValidationIssue`, `ConfigDiff` / `ConfigPlan`, `ConfigOptions`, `ConfigExportOptions`, `ConfigApplyResult`.
- Functions:
  - `exportConfig(const DeviceSnapshot &, const ConfigExportOptions &)`
  - `validateConfig(const DeviceSnapshot &, const DeviceConfig &, const ConfigOptions &)`
  - `planConfig(const DeviceSnapshot &, const DeviceConfig &, const ConfigOptions &)`
  - `applyConfig(Client &, const DeviceSnapshot &, const DeviceConfig &, const ConfigOptions &)`
- Unit tests in `host/test/test_config.cpp` (pure planning/validation/ordering/cycle/pin-swap/clamping-rejection/export cases) plus live vdev apply/rollback tests.

**Done when** `host-win`, `host-linux`, `agent`, and `ros` build and pass with zero warnings, installed `libdrivelink` consumers remain free of JSON/YAML dependencies, and unit + vdev tests cover full apply, `--patch`, `--reset-defaults`, pin swaps, limit raises/lowers, dependency cycles, and mid-apply `DISCARD_PENDING` rollback.

### Step 2 (`DC2`): Pinned `rapidyaml` & `host/config_yaml` serializer

- Add `host/cmake/Yaml.cmake` fetching a version- and SHA256-pinned `rapidyaml` release (MIT), included as `SYSTEM` headers so third-party warnings never fail `-Wall -Wextra -Werror` on MinGW 13.1 (`-O0` and `-O3` release) or Linux GCC 15/13.
- Add `host/config_yaml/` (static library `drivelink_config_yaml`, with `COLCON_IGNORE` and `README.md`) converting between YAML/JSON text and `drivelink::DeviceConfig`:
  - Parse YAML 1.2 (including JSON) with clear line/column error messages and key-path context.
  - Flatten nested param maps (`vel: { kp: 0.4 }` → `vel.kp`) and reject duplicate flattened keys.
  - Emit clean, deterministic, human-readable YAML in canonical section/key order.
- Update `host/notices/notices.json`, `gui/notices/notices.json`, `gui/notices/licenses/`, and regenerate both `THIRD_PARTY_NOTICES.txt` files via `gen_notices.py --write`.
- Unit tests in `host/test/test_config_yaml.cpp` covering YAML/JSON parsing, nested vs flat params, null handling, syntax errors, and `export` → emit → parse round-trips.

**Done when** `host-win`, `host-linux`, `deb`, and `installer` checks pass with zero warnings and notices verification succeeds.

### Step 3 (`DC3`): `drivelink-cli config` subcommands, integration tests, and docs

- Wire `config export | validate | diff | apply` into `host/cli/src/main.cpp` and `host/cli/src/format.cpp` (readable text + `--json` envelopes, preserving existing CLI golden/replay fixtures untouched).
- Implement `--reboot` and `--wait` (polling/reconnecting over serial, TCP, or agent until the device is back and checking all instances are `Running`).
- Integration tests in `host/test/` exercising:
  - Direct vdev (TCP and Linux PTY): `export` → edit → `validate` → `diff` → `apply --reboot --wait` → verify idempotent second `diff` (zero changes).
  - Live-only update (no reboot required) vs layout + `limits.*` update (reboot required) vs `--patch` overlay vs `--reset-defaults`.
  - Error guards (Armed, pending edits + `--discard-pending`, `device.id` mismatch + `--force-device`, out-of-range rejection, bad pin / rollback).
  - Through `drivelink-agent` (`--device`, `--token`, `--lease` and lease refusal without `--lease`).
- Update `host/cli/README.md`, `host/libdrivelink/README.md`, and add a user guide page under `guide/` showing the declarative workflow end-to-end.
- Run a bare-board smoke check (mock modules / factory layout restoration per [docs/bench.md](../docs/bench.md)) and record it in [hardware-log.md](hardware-log.md).

**Done when** `python tools/validate.py quick`, `host-linux`, `agent`, `ros`, `deb`, `installer`, and `guide` pass with zero warnings, and the bare-board smoke check passes and restores the board cleanly.

## Not in this issue

- GUI import/export dialogs (will reuse `libdrivelink`'s config engine and `host/config_yaml` in a follow-up).
- Module-specific shorthand/macros (such as a `motors:` preset).
- Ad-hoc named references on existing low-level commands (`drivelink-cli get/set left_ctrl.vel.kp`).
- Offline validation without a connected device (descriptors live on the device; cached descriptor profiles could be added later).
- Wire protocol or firmware changes.
