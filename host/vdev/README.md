# drivelink-vdev

A virtual ESP using the actual firmware `core`, native `core_os` and `mod_mock`.
One config caller runs the existing `CommandService`; real loop threads execute
mock motor/feedback and IMU logic. It has no physical HALs.

`drivelink-vdev --port 8765 --state DIR` listens on loopback TCP and prints
`READY tcp://127.0.0.1:8765`. Port 0 chooses an ephemeral port. Linux `--pty`
prints `READY serial:///dev/pts/N` and serves a raw PTY instead. It keeps the
slave open across CLI invocations. One client owns configuration at a time;
extra TCP connections are closed. Reboot replies are written before the core
is rebuilt, loops/boot clock/diagnostics reset, and TCP is disconnected.
PTY remains open across reboot. Command reply drains have a two-second deadline
from enqueue. A failed TCP drain closes the connection and cancels the reboot.
A failed PTY drain cancels/drops queued replies and effects, retaining the master
so a new CLI can reconnect. Idle PTY streams may wait for a reader; their queue
and in-flight buffer remain bounded, and later status samples replace pending ones.

The vdev implements core v9, time v1, status and diag v3. CPU load is unavailable (255).
It uses fixed 254-slot diagnostic identities and a bounded record queue; slow
streams use latest-wins queueing. Control loops never allocate diagnostics.
There is no synthetic host-specific module table; the actual generated mock
definitions are served by the core.

Device id defaults to `02444c000001`, stable across reboot and reset.
`--device-id HEX12` simulates distinct boards. Names use real core storage.

## GUI scenario controls

These are explicit native test controls, not device wire operations. Without
the options below the normal vdev behavior is unchanged.

`--safe-mode` seeds the native crash counter before boot, using the real core
safe-mode path. `--scenario-file PATH` polls a local control file. Replace it
with one line, `<sequence> <action>`, using an increasing unsigned sequence;
an already consumed sequence is ignored. For example, write `1 fault`, then
replace it with `2 fault-clear`. Each accepted line prints `SCENARIO`.

| Action | Effect |
|---|---|
| `fault`, `fault-clear` | Raise/clear the real `core.LoopStalled` condition for loop 0; acknowledgement still uses the real core service |
| `stall`, `resume` | Stop/resume transport RX/TX while the core keeps running |
| `slow N` | Delay ordinary core/diag request dispatch by 0..1000 ms; fast DISARM and time/status keep running; at most four delayed requests per peer |
| `reboot` | Rebuild the core, closing TCP or retaining the PTY and emitting Boot |
| `drop-config-reply N` | Suppress the Nth following CMD_CONFIG reply after the real mutation, once (0 disables, maximum 32) |
| `drop-disarm N` | Ignore the next N fast DISARM requests, including their replies (maximum 32) |
| `drop-arm-reply N` | Suppress the Nth following ARM reply after applying the real operation, once (maximum 32) |
| `stats` | Write `PATH.stats`: received prefix/value command-frame counts, fast DISARM request count, configured command group IDs/loops/widths |

Counters in the sidecar let GUI tests prove they sent no input value frames
and did not adopt another owner's groups. The GUI scenario runner
`host/gui/tests/scenarios.py` creates isolated temporary state/control files;
see [GUI screenshots and backend checks](../gui/README.md). It never sends
these injections to a supplied physical endpoint.

## Persistence and crashes

Only one process can own a state directory (OS-level lock released on exit).
`kv/<namespace>.<key>.blob` holds each KV blob separately. Each write uses a
flushed temporary file and atomic replace; POSIX also fsyncs the parent directory,
Windows uses MoveFileExW with WRITE_THROUGH. The core's existing A/B generation
algorithm commits the layout last, preserving a previous complete generation
if a process dies mid-save. Temporary files left by a kill are ignored.
This simulates process termination and host filesystem durability; it cannot
prove the behavior of a particular physical storage device on power loss.

`boot.state` stores the crash counter and a running marker with version/checksum.
Kill/abort without orderly shutdown counts as a crash on the next start; three
consecutive crash resets enter the real core safe mode. Its 30-second clear
timer updates the file too. SIGINT/SIGTERM perform a clean shutdown on Linux.
REBOOT and EXIT_SAFE_MODE are non-crash resets. Windows TerminateProcess counts
as a crash, including test cleanup. Do not delete/edit boot.state while running.
Malformed boot metadata fails startup explicitly.

`--allow-crash` enables the core's bench-only crash operation; default is off.
`--fail-writes-after N` fails persistent KV writes after N successful operations,
for save/recovery integration tests. Neither option affects hardware firmware.
The native in-memory unit-test store retains its deterministic failed/torn-write
injection and remains the default unless a file directory is attached.

Loop instances, the factory layout and safe mode use the same core as firmware.
Native real-time guard violations are counted and reported on stderr; safe mode
starts no loop threads. LP format-2 layouts load intact with an empty name;
older formats fall back to factory loops. SAVEs now write format 3.

Group command frames use the real core RX mailbox and disarmed gate. The
config timer drains subscription snapshots; stream coalescing compares the
channel so one group cannot replace another group or the status stream.


F4b runs the same arming state, heartbeat/command monitor and mock emergency
force as the board. Native latched diagnostics share their fixed atomic table
with the safety interlock and reporter; reporting uses bounded storage. Native
scheduling can jitter more than a board: use slower loop rates (for example
100 Hz) for desktop arming tests. `--allow-crash` also enables BenchStall; a
native stall exercises the monitor, while Task WDT panic-reset is device-only.

On Windows, vdev requests 1 ms timer resolution for its process before starting
the core and releases it after all core threads stop. Startup fails explicitly
if Windows refuses the request. This prevents coarse default waits from
dominating short loop periods, including on CI. It does not guarantee real-time
scheduling; elapsed-period accounting, overruns and all control/safety rules are
unchanged. The shared native backend and hardware firmware are unaffected.

F5a-1 also registers the actual `controller.cascade_pid` implementation (0x20).
It drives `actuator.mock` through the same interface as future PWM hardware and
reads quantized counts/position/filtered velocity from `feedback.mock`. Plant
params include deadband, saturation, selectable one-tick delay and optional
seeded encoder noise. The real-time guard covers controller and mock ticks.
`host/test/motor_smoke.py` runs the same CLI control scenario on TCP and UART.

F6's `imu.mock` uses the shared pure IMU state/calibration codec. Persistent
calibration, paused actions and modes run through the actual core; vdev has no
I2C backend and does not register `imu.bno055`. The generic CLI/host discovers
all module metadata; no module table is compiled into libdrivelink.

F7b runs the actual per-loop test generator, capture-start trigger, terminal
events and aggregate status flag. It registers no separate waveform emulator.
Host integration runs step/chirp and handoff tests at 50 Hz; deterministic
native tests cover exact timing and concurrent loops. Its TCP endpoint also
supports the test-only lost-ARM-response proxy, which verifies cleanup DISARM
in both `testgen-run` and `cmd-run`.

F8 time requests use the shared exact codecs, take RX time at dispatch, and
patch TX time/CRC only when their queued write begins. They have the same
reliable-response deadline as core/diag replies, including on PTY. Malformed
requests are dropped and counted. See `docs/protocol/time.md`.

## OTA simulation (F9)

The actual core OTA service operates on a native two-slot backend. `ota.bin`
stores checked slot/build identities and boot state in the exclusive state
directory. A fresh baseline uses the compiled source build ID (zero-extended
for the simulated full hash). A candidate's descriptor supplies its simulated
identity/version, while GET_INFO keeps a -vdev suffix. This preserves existing
physical-hardware preflight refusals.

FINISH prepares only; explicit SYSTEM REBOOT commits after its reply is written.
The peer fences later CoreCommand dispatch while an after-reply action is
queued, replying Busy/StagingBusy; fast DISARM/time remain available. A failed
drain/cancelled queue cannot commit, and a failed select keeps the service
gated for STATUS-based recovery. Pending boot, confirmation and the next-reset
rollback are modeled across soft/process restarts. Corrupt metadata fails
closed. Uploaded bytes/handles are not resumed after reset.

The same native executable continues running after a simulated update. It
cannot establish ESP image execution, eFuse compatibility, physical flash
timing or power-loss behavior; those need the bare board. Shared integration
and failure-proxy cases are documented in the host README and OTA spec.
