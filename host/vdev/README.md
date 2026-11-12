# drivelink-vdev

A virtual ESP using the actual firmware `core`, native `core_os` and `mod_mock`.
One config caller runs the existing `CommandService`; real loop threads execute
mock motor/feedback and IMU logic. It has no legacy managers or physical HALs.

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

The vdev implements core v1, status and diag v3. CPU load is unavailable (255).
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
