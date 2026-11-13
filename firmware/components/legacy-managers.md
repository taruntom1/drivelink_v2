# The legacy managers

`imu_bmo_055` and `time_sync` are the protocol-facing managers from before the overhaul. They keep working until their step ports them to the core: `imu_bmo_055` in F6, `time_sync` in F8. Each step deletes its part of this file, and F8 deletes the file. (`motor_manager` and `motor_controller` were deleted in F5b; `main`, its `legacy-final` tag (F10) and git history keep them.)

Don't improve this code. Fix it only if it blocks the current step. The firmware-wide rules in [firmware/CLAUDE.md](../CLAUDE.md) apply to it. Paths are relative to the repository root.

## Structure

- Each manager registers UART callbacks and owns its own tasks.
- Each keeps its payload codec in a pure module with no ESP-IDF dependency (`imu_protocol`, `time_sync_protocol`, both private), built on `drivelink::protocol::wire::Reader` / `Writer`. The managers decode and encode only through these, so the native tests cover the wire format.
- RX callbacks forward configure commands to the manager's own config task queue.
- **Wake-up frames:** a manager wakes its config task with a dummy queue item on shutdown. The config task must re-check the stop flag before dispatching anything it receives.
- The legacy managers don't use NVS.

## Wire format

The wire format is defined by the code and its native tests. The IMU also has `firmware/components/imu_bmo_055/docs/protocol-api-guide.md`. The READMEs that were deleted are in git history: `git show 7a4d693:components/<name>/README.md`.

- **MSG_TYPEs** (defaults from the registry):
  - time_sync: `0x20` / `0x60`.
  - IMU: `0x22` / `0x42`, its stream on `0xBF`.
  - The IMU stream sits at the top of the command group range until F6 removes it.
- **Versions:** the IMU is at v6, time_sync at v2. v6 changed no payload: it came with the F2 move of the stream channels, and v5 with the F1b framing.
- **IMU configure commands:**
  - Commands and responses carry a host-chosen `seq` byte right after the version, echoed in the response.
  - Every response carries `result` and then a `detail` byte (the manager's `ResultDetail`) saying why it failed. The manager methods return the pair as a `Result`.
  - A command that arrives while the config queue is full is answered from the RX callback (`QueueFull`) instead of being dropped.
  - A response to a header that could not be parsed uses `0xFF` (`kUnparsedHeaderByte`) for the subcommand.
  - Configure bodies must have their exact size.
- **IMU:** `SetStreamTiming` (`0x02`) sets the sample and telemetry periods. Its response appends the applied values after the 5-byte header.
- **Timestamps:** IMU stream timestamps are wall-clock µs (`gettimeofday`, corrected by time_sync), latched at the sampling instant.
- **time_sync:** its ESP-initiated request carries a `u16 request_id`.

## IMU sampling loop

- A `TaskTimer` wakes the IMU stream task every sample period. A packet is sent every N samples, with N derived from the telemetry period. The loop does not depend on `CONFIG_FREERTOS_HZ`.
- The tick reads the stream settings from atomics and only try-locks the `Bno055Device` mutex. A busy device skips the tick and counts it.
- Late, busy, read-error and encode-error counts are diag counters (`DIAG_COUNT`). Nothing is logged per tick.
- The timer is stopped while the stream is disabled and during an operation-mode switch.

## IMU vendor code and naming

- `firmware/components/imu_bmo_055/vendor/bno055.c` / `bno055.h` are the Bosch driver. Don't edit them. `vendor/` is a private include dir; only `Bno055Device` uses it.
- The component and namespace are spelled `imu_bmo_055`, but the part is the BNO055.
