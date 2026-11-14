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

The vdev implements core v8, time v1, status and diag v3. CPU load is unavailable (255).
It uses fixed 254-slot diagnostic identities and a bounded record queue; slow
streams use latest-wins queueing. Control loops never allocate diagnostics.
There is no synthetic host-specific module table; the actual generated mock
definitions are served by the core.

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
starts no loop threads. Old layout blobs fall back to the factory loops.

Group command frames use the real core RX mailbox and disarmed gate. The
config timer drains subscription snapshots; stream coalescing compares the
channel so one group cannot replace another group or the status stream.


F4b runs the same arming state, heartbeat/command monitor and mock emergency
force as the board. Native latched diagnostics share their fixed atomic table
with the safety interlock and reporter; reporting uses bounded storage. Native
scheduling can jitter more than a board: use slower loop rates (for example
100 Hz) for desktop arming tests. `--allow-crash` also enables BenchStall; a
native stall exercises the monitor, while Task WDT panic-reset is device-only.

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
