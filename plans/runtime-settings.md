# Runtime settings (issue 105)

Plan for [issue 105](https://github.com/taruntom1/drivelink_v2/issues/105): change at
run time, through the CLI, GUI and agent, what now needs a Kconfig edit and a
rebuild. Five PRs, one per step, in order. State: **steps 1 to 3 done**; steps 4 and 5 planned.

## Why it fits the current design

The device already describes its params, the CLI and agent already forward any
param, and module params already support "staged until SAVE and reboot". Three
things are missing:

1. **System params cannot be `reboot_required`.** SET answers `Unsupported` and
   there is no pending copy; GET_PENDING returns the running value.
2. **Limits are fixed before settings are read.** `Core`'s constructor sizes
   everything from `CoreConfig`; `Core::boot()` loads NVS later. The UART opens
   earlier still.
3. **The GUI has no editor for system params**, not even `watchdog_ms`.

## Decisions (2026-11-24, user)

| Topic | Decision |
|---|---|
| Scope | User-facing settings only. Task stacks, priorities, queues and buffers stay in Kconfig |
| PWM frequency, glitch filter | Per motor and per encoder, as module params |
| PWM range | 5 to 100 kHz, so the PWM period stays below the shortest loop period (0.5 ms) and the no-flip reversal rule holds |
| Kconfig | A migrated option is removed. The YAML owns default and range |
| Capacity limits | SET refuses what cannot fit in RAM; safe mode boots with default limits and drops no saved instance |
| UART link | Baud and pins, with a trial: 30 s for a baud change, 120 s when a pin changes; the device then reboots to the old link |
| Link confirm | New op LINK_CONFIRM, sent by the host tool after it reconnects to the same device. A reconnecting agent sends it too |
| Host baud | An enum of 8 rates; hosts auto-detect by default, a fixed baud stays as an override |
| Safe mode, FACTORY_RESET | Both keep the confirmed link |
| GUI | A settings card on the Device page (Figma first) |
| CLI | Numeric references only; no `settings` command |
| Wire | Core v10 to v11 once, in step 1, with LINK_CONFIRM and its details reserved |

## What moves

| Kconfig option | Becomes | Applies | Step |
|---|---|---|---|
| `CORE_MAX_LOOPS`, `CORE_MAX_INSTANCES`, `CORE_SUBSCRIPTION_GROUPS`, `CORE_COMMAND_GROUPS`, `CORE_GROUP_ENTRIES`, `CORE_MAX_LINK_VALUES`, `CORE_CAPTURE_MAX_REFS` | the existing `limits.*` items (same ids), now writable | reboot | 2 |
| `CORE_CAPTURE_MAX_KB` | new `limits.capture_kb` | reboot | 2 |
| `DIAG_COUNTER_PERIOD_MS`, `DIAG_REPORTER_POLL_MS`, `DRIVELINK_STACK_LOG_PERIOD_S` | new `diag.*` system params | live | 2 |
| `MOTOR_HAL_PWM_FREQ_HZ` | `actuator.pwm_dir` `pwm_freq_hz` | reboot | 3 |
| `ENCODER_HAL_GLITCH_FILTER_NS` | `feedback.quadrature` `glitch_ns` | reboot | 3 |
| `UART_COMM_BAUD_RATE`, `UART_COMM_TX_PIN`, `UART_COMM_RX_PIN` | `link.baud`, `link.tx_pin`, `link.rx_pin` | reboot, trial | 5 |

**Stays in Kconfig:** `UART_COMM_PORT_NUM`; `UART_COMM_MAX_PAYLOAD` (sizes static
memory); all task stacks and priorities, queue depths and buffer sizes; the MCPWM
timer resolution; the PCNT limits; `UART_COMM_RX_TIMEOUT_SYMBOLS`;
`CORE_STATUS_CPU_LOAD`, `DIAG_CONSOLE_LOG` and `CORE_BENCH_CRASH` (build features).
ESP-IDF's own options (CPU clock, flash, PSRAM, partitions) are out of scope.

## Step 1: pending system settings, core v11

The base for the other steps; no Kconfig option moves yet.

- **Firmware.** A pending copy for `reboot_required` system params, beside the
  layout's. SET stages it and answers `PendingReboot`; GET is the running value,
  GET_PENDING the one the next boot uses; SAVE writes it; DISCARD_PENDING drops it;
  REVERT still touches live params only. The whole pending layout is validated
  against the pending system values.
- **Boot order.** Split boot into (a) decide safe mode and load NVS, (b) build
  `CoreConfig` from the saved values, (c) construct `Core`. The crash counter must
  go up **before** anything is sized from settings; today it goes up after the
  constructor, so a bad limit would never reach safe mode. `main`, vdev and the
  native tests share the new path. The generation is loaded once, not twice.
- **Wire.** Core v11. Reserve `SystemOp::LinkConfirm` (11, `Unsupported` until
  step 5) and new details: link trial active, no trial to confirm, not enough
  memory. Existing detail numbers stay. Update `docs/protocol/core.md`,
  `definitions.md`, the Python HIL reference, vdev, host and ROS together.
- **Host.** `DeviceSnapshot` gains `pending_system_params`; the GUI's pending-change
  list and Tune's session fingerprint account for it.
- **Tests.** Native: stage, get, save, reboot, discard, revert, power loss during
  SAVE, crash before and after construction. Generator fixture with a
  `reboot_required` system item. Host model tests. Integration: CLI and agent
  against vdev across a reboot.

**Done when** a test-only reboot-required system param survives SAVE and reboot over
TCP and through the agent, and an unconfirmed pending value never changes the
running one.

## Step 2: capacity limits and diagnostic timing

- **Limits** become writable, persisted, `reboot_required`, with the YAML ranges of
  today's Kconfig. GET stays the effective value that hosts size themselves by.
  `limits.group_entries` and `limits.capture_refs` keep the payload cap;
  GET_INFO's `capture_capacity` stays the effective ring size.
- **Checks on SET.** A limit below what the pending layout uses is refused
  (`Capacity`). Raising a limit and adding instances can be done in one pending
  session: ADD checks the pending limit.
- **RAM check.** Estimate the cost of all pending limits and loop stacks (group
  slots about 3.9 KB each, a loop about 7 KB, link buffers, UART stream slots)
  against the internal heap recorded at boot before `Core` is built, minus a
  reserve. Measure the costs on the board and keep the estimate conservative.
  ADD of a loop and a loop `stack_bytes` change use the same check. Allocate
  group slots one by one: 31 in one array needs a 120 KB contiguous block.
- **Safe mode** uses default limits, but the record table is sized for the saved
  layout, so no saved instance is dropped by the next SAVE. GET shows the defaults
  in use, GET_PENDING the saved values, so the user can lower them.
- **Diagnostic timing.** `diag.counter_period_ms`, `diag.poll_ms`,
  `diag.stack_log_s`: live. The core stays pure: it hands values to the reporter
  through the `core_os` seam.
- **Remove** the moved Kconfig options; rewrite the "tunables belong in Kconfig"
  rule in `firmware/components/CLAUDE.md`.
- **Hosts** read limits that may now differ per device: check the GUI, ROS plugin,
  CLI and agent for a hard-coded 4, 8, 16 or 64.

**Done when** native tests cover each refusal and the safe-mode paths, and on the bare
board: 6 loops and 12 + 12 groups run after SAVE and reboot with more than 1 KB free
in every task; an impossible set is refused; a forced crash loop ends in safe mode
with every instance still listed.

## Step 3: per-motor PWM and per-encoder filter

- **PWM.** `pwm_freq_hz` u32, 5000..100000, default 25000. Each motor gets its own
  MCPWM timer (2 groups of 3 timers and 3 operators: 6 motors, as now). All timers
  of a group keep one resolution. Force, stop and release code does not change.
  Document the effective duty steps: 400 at 25 kHz, 100 at 100 kHz.
- **Filter.** `glitch_ns` u16, 0..10000, default 1000, 0 = off.
- **Seams.** The values go through `openPwmDir` / `openQuadrature`; the fakes record
  them. Update the `mod_motor` README: the "loop period longer than PWM period"
  condition is now guaranteed by the range, with a test tying the minimum to the
  loop's 2 kHz maximum.

**Done when** native tests pass, and on the bare board motors at different frequencies
start, run and stop, with 6 allocated and a 7th refused. Real-motor and scope checks
become a deferred row in [host-status.md](host-status.md).

## Step 4: GUI settings card and docs

- **Design first.** A Settings card on the Device page in Figma (`gui-design-step`):
  live, staged, unsaved, locked while armed, refused, read-only observer, safe
  mode, narrow window.
- **Card.** Every writable system param from the descriptors, grouped by name prefix,
  nothing hard-coded. It reuses Layout's field, parser, range and pending code. The
  Layout rules apply: each SET is sent once, then read back; staged and live values
  never look alike; Esc and DISARM cancel an open edit; the lease rule for live
  edits while another client holds the lease holds. The Limits card stays, for the
  effective values.
- **Layout page** counts use the pending limits.
- **CLI.** No new command. Document `get`, `get-pending`, `set`, `save` and `reboot`
  with the system references, and the agent form with `--lease`.
- **Guide.** A short "Device settings" page.

**Done when** GUI tests cover each state locally and through the agent, and changing a
limit works by hand on vdev and on the bare board.

## Step 5: link trial and baud auto-detect

Riskiest step: an error here can cut off a device.

- **Params.** `link.baud` is an enum: 115200, 230400, 460800, 921600 (default),
  1000000, 1500000, 2000000, 3000000. `link.tx_pin` and `link.rx_pin` default to
  43 and 44. Refused: TX equal to RX, a pin outside the GPIO policy, a pin any
  module uses. Modules cannot claim a running, staged or fallback link pin. GPIO 43
  and 44 stay reserved always: the ROM prints on 43 at every reset, which would
  drive a motor pin.
- **Storage.** Link settings live in their own NVS namespace: `confirmed` and one
  `trial` record. FACTORY_RESET leaves it alone; the SAVE that commits a link change
  writes the trial record last.
- **Boot.** New order: diag, crash/safe-mode decision, NVS, link settings, UART,
  core, CoreLink, Reporter. A trial is marked as used **before** the UART opens, so
  a crash, power loss or timeout always comes back on `confirmed`. A UART that fails
  to open on a trial falls back at once. Safe mode uses `confirmed`.
- **Trial.** 30 s, or 120 s when a pin changes. ARM is refused. LINK_CONFIRM stores
  the trial as confirmed and answers only after the write. At the timeout the device
  DISARMs, reports a diag code and reboots. A link change is refused while an OTA
  image is unconfirmed, and OTA BEGIN is refused during a trial. GET_INFO reports the
  trial and its remaining time.
- **Host library.** Open with `baud = auto`: last known rate, 921600, then the others,
  bounded, sending only GET_INFO. A fixed baud tries only that one. A reboot
  expectation covers the trial. LINK_CONFIRM goes out once, only after the device id
  matches; a lost reply is resolved by reading GET_INFO, never re-sent blindly.
- **Agent.** `baud` accepts `"auto"` (new default). It confirms a trial after
  reconnecting to the same device id and keeps the rate it found. LINK_CONFIRM from
  clients is an exclusive operation.
- **CLI.** `--baud auto` default, and `link-confirm`. `reboot` stays a plain reboot:
  it prints that a trial is pending and the command to confirm it.
- **GUI.** The card warns before the reboot, shows the countdown and the outcome
  (confirmed, reverted, not reachable), and says to move the wires on a pin change.
- **Recovery.** Document erasing the NVS partition over USB with esptool, which clears
  all settings. Also document that firmware older than this step ignores the link
  settings and comes up at 921600 on 43/44.

**Done when** native tests cover power loss at every write, timeout, crash, UART failure
and late confirm; vdev over a PTY shows auto-detect, confirm and revert through the CLI,
GUI and agent; and on the bare board (CH343) every rate the adapter supports passes a
baud trial, an unconfirmed one reverts, and a bad pin reverts. A real pin change and the
Pi UART become deferred rows.

## For every step

- The [checks before a PR](../docs/development.md#checks-before-a-pr), zero warnings;
  generated files, READMEs and
  `docs/protocol/` in the same PR; `cli-golden.json` follows the descriptor hash.
- Delete `firmware/sdkconfig` and `firmware/test/uart_comm/sdkconfig` after a Kconfig
  removal. The UART test app sets its own pins and baud from step 5.
- Board runs go in [hardware-log.md](hardware-log.md).
- Steps 2, 3 and 4 may be reordered after step 1; step 4 is most useful after step 2.
  Step 5 needs 1 and 4.

## Not in this issue

Other UART ports, task and queue internals, runtime console log level, settings by name
in the CLI (see [issue 100](https://github.com/taruntom1/drivelink_v2/issues/100)), flash
sizes ([issue 103](https://github.com/taruntom1/drivelink_v2/issues/103)).
