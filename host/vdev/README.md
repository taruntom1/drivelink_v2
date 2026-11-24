# drivelink-vdev

A virtual device that runs the firmware [core](../../firmware/components/core/README.md) on the native [core_os](../../firmware/components/core_os/README.md) backend with [mod_mock](../../firmware/components/mod_mock/README.md). It has no hardware drivers. Host integration, GUI, agent and ROS checks use it in place of a board.

## Run

```sh
drivelink-vdev [--state DIR] [--port PORT | --pty] [--allow-crash] [--device-id HEX12] [--fail-writes-after N] [--scenario-file PATH] [--safe-mode] [--test-setting]
```

| Option | Effect |
|---|---|
| `--state DIR` | State directory (default `vdev-state`); see [Persistence and crashes](#persistence-and-crashes) |
| `--port PORT` | Loopback TCP port (default 8765; 0 picks an ephemeral port). Prints `READY tcp://127.0.0.1:PORT` |
| `--pty` | Linux only: serve a raw PTY instead of TCP. Prints `READY serial:///dev/pts/N` |
| `--device-id HEX12` | Device id (default `02444c000001`), stable across reboot and reset; use distinct ids to simulate several boards |
| `--allow-crash` | Enables the core's bench-only crash and BenchStall operations (default off) |
| `--fail-writes-after N` | Fails persistent KV writes after N successful operations, for save and recovery tests |
| `--scenario-file PATH`, `--safe-mode` | GUI test controls; see [Scenario controls](#scenario-controls) |
| `--test-setting` | Adds one test-only `reboot_required` system param, `test.setting` (0:0:32512, u16, 1..100, default 10), for the pending-settings tests |

None of these options affect hardware firmware.

## What runs

- The real core services, generated mock definitions and loop threads, including the factory layout, safe mode, arming, heartbeat and command monitor, mock emergency force, capture, the per-loop test generator and core events. There is no host-specific module table.
- `mod_mock` modules: `actuator.mock` and `feedback.mock` with plant parameters (deadband, saturation, optional one-tick delay, seeded encoder noise), `imu.mock` on the shared IMU state and calibration codec, `fusion.mock`, and the real `controller.cascade_pid`. vdev does not register `imu.bno055` or any pin-based module.
- Core v11, time v1, status and diag v3. CPU load reports 255 (unavailable). Diagnostics use fixed 254-slot identities and a bounded record queue; control loops never allocate them.
- Time requests use the shared codecs, take RX time at dispatch, and patch TX time and CRC when their queued write begins. Malformed time requests are dropped and counted. See the [time contract](../../docs/protocol/time.md).
- Native real-time guard violations in loop, controller and mock ticks are counted and reported on stderr. Safe mode starts no loop threads.

## Transport

One client owns the device at a time; extra TCP connections are closed. A PTY keeps its slave open across CLI invocations and reboots.

Command, diag and time replies have a two-second drain deadline from enqueue. A failed TCP drain closes the connection and cancels a pending reboot. A failed PTY drain drops queued replies and effects but keeps the master, so a new client can reconnect. Idle PTY streams may wait for a reader; their queues stay bounded and newer status samples replace pending ones. Slow streams use latest-wins queueing; stream coalescing compares the channel, so one group never replaces another group or the status stream.

REBOOT writes its reply before the core is rebuilt; loops, boot clock and diagnostics then reset. Over TCP, vdev then disconnects gracefully: it sends FIN, discards further input (frames pipelined behind REBOOT are lost, as on a device) and closes when the client closes or after one second. A new client can connect during that second. A plain close with unread input would send RST, which on Windows discards the client's unread reply.

## Scenario controls

These are native test controls, not wire operations; without the options below vdev behaves normally.

`--safe-mode` seeds the crash counter before boot so the real core enters safe mode. `--scenario-file PATH` polls a local control file. Replace its content with one line, `<sequence> <action>`, using an increasing unsigned sequence; an already consumed sequence is ignored. For example, write `1 fault`, then replace it with `2 fault-clear`. Each accepted line prints `SCENARIO`.

| Action | Effect |
|---|---|
| `fault`, `fault-clear` | Raise/clear the real `core.LoopStalled` condition for loop 0; acknowledgement still uses the real core service |
| `stall`, `resume` | Stop/resume transport RX/TX while the core keeps running |
| `slow N` | Delay ordinary core/diag request dispatch by 0..1000 ms; fast DISARM and time/status keep running; at most four delayed requests per peer |
| `reboot` | Rebuild the core, closing TCP or retaining the PTY and emitting Boot |
| `drop-config-reply N` | Suppress the Nth following CMD_CONFIG reply after the real mutation, once (0 disables, maximum 32) |
| `drop-disarm N` | Ignore the next N fast DISARM requests, including their replies (maximum 32) |
| `drop-arm-reply N` | Suppress the Nth following ARM reply after applying the real operation, once (maximum 32) |
| `drop-reboot N` | Ignore the next N SYSTEM REBOOT requests: neither applied nor answered (maximum 32) |
| `drop-action-reply N` | Suppress the Nth following ACTION RUN reply after the action ran, once (maximum 32) |
| `loop-stall LOOP MS` | Stall one tick of a running loop for 1..10000 ms (the bench stall, without its wire gate), so its sources age |
| `stats` | Write `PATH.stats`: received prefix/value command-frame counts, fast DISARM, REBOOT and ACTION RUN request counts, configured command group IDs/loops/widths |

The `stats` counters let GUI tests prove they sent no input value frames and did not adopt another owner's groups. The GUI scenario runner `host/gui/tests/scenarios.py` creates isolated temporary state and control files and never sends these injections to a supplied physical endpoint; see [GUI screenshots and backend checks](../gui/README.md).

## Persistence and crashes

Only one process can own a state directory; an OS-level lock is released on exit. `kv/<namespace>.<key>.blob` holds each KV blob, written through the native [file store](../../firmware/components/core_os/README.md) by flushed temporary file and atomic replace. Temporary files left by a kill are ignored. The core's A/B generation commit preserves the previous complete configuration if the process dies mid-save. This models process termination and host filesystem durability, not a physical storage device on power loss.

`boot.state` stores the crash counter and a running marker with version and checksum. A kill or abort without orderly shutdown, including Windows `TerminateProcess` in test cleanup, counts as a crash on the next start; three consecutive crash resets enter safe mode, and the 30-second clear timer updates the file too. SIGINT and SIGTERM shut down cleanly on Linux. REBOOT and EXIT_SAFE_MODE are not crashes. Do not delete or edit `boot.state` while vdev runs; malformed boot metadata fails startup.

Without a state directory attached, native unit tests keep the in-memory store with its deterministic failed and torn-write injection.

## OTA simulation

The real core OTA service runs on a native two-slot backend. `ota.bin` in the state directory stores checked slot and build identities and boot state. A fresh baseline uses the compiled source build ID, zero-extended to the simulated full hash. A candidate's descriptor supplies its simulated identity and version, while GET_INFO keeps the `-vdev` suffix so hardware-only preflights still refuse.

FINISH only prepares; an explicit SYSTEM REBOOT commits after its reply is written. While that after-reply action is queued, later core commands get `Busy/StagingBusy`; fast DISARM and time stay available. A failed drain or cancelled queue cannot commit, and a failed select keeps the service gated for STATUS-based recovery. Pending boot, confirmation and next-reset rollback survive soft and process restarts; uploaded bytes and handles do not. Corrupt metadata fails closed. See the [OTA simulator notes](../../docs/protocol/ota.md#partition-table-and-simulator) and the [host OTA campaign](../README.md).

## Desktop timing

On Windows, vdev requests 1 ms timer resolution before starting the core and releases it after all core threads stop; startup fails if Windows refuses. This keeps coarse default waits from dominating short loop periods but does not make scheduling real-time. Period accounting, overruns and all control and safety rules are unchanged.

Native scheduling jitters more than a board. Use slower loop rates for desktop arming tests (for example 100 Hz); host integration runs testgen at 50 Hz, and deterministic native tests cover exact timing.

## Limits

vdev cannot establish ESP image execution, eFuse compatibility, flash timing or power-loss behavior, and a native BenchStall exercises the monitor but not the Task WDT panic reset. Those checks need the bare board; see the [bench instructions](../../docs/bench.md).
