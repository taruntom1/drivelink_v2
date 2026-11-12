# The legacy managers

`motor_manager`, `imu_bmo_055` and `time_sync` are the protocol-facing managers from before the overhaul. They keep working until their step ports them to the core: `motor_manager` goes in F5b, `imu_bmo_055` in F6, `time_sync` in F8. Each step deletes its part of this file, and F8 deletes the file.

Don't improve this code. Fix it only if it blocks the current step. The firmware-wide rules in [firmware/CLAUDE.md](../CLAUDE.md) apply to it. Paths are relative to the repository root.

## Structure

- Each manager registers UART callbacks and owns its own tasks.
- Each keeps its payload codec in a pure module with no ESP-IDF dependency (`motor_protocol`, `imu_protocol`, `time_sync_protocol`, all private), built on `drivelink::protocol::wire::Reader` / `Writer`. The managers decode and encode only through these, so the native tests cover the wire format.
- `motor_controller` combines one motor, one encoder and the PIDs into a controller with open-loop, velocity and position modes. Its velocity estimate is the pure `VelocityEstimator`.
- RX callbacks forward configure commands to the manager's own config task queue. Setpoint streams go into a mailbox that the motor update task applies on its next tick.
- **Wake-up frames:** a manager wakes its config task with a dummy queue item on shutdown. The config task must re-check the stop flag before dispatching anything it receives.
- The legacy managers don't use NVS.

## Wire format

The wire format is defined by the code and its native tests. The IMU also has `firmware/components/imu_bmo_055/docs/protocol-api-guide.md`. The READMEs that were deleted are in git history: `git show 7a4d693:components/<name>/README.md`.

- **MSG_TYPEs** (defaults from the registry):
  - time_sync: `0x20` / `0x60`.
  - motor_manager: `0x21` / `0x61`, the setpoint stream on `0xBD` and telemetry on `0xBE`.
  - IMU: `0x22` / `0x42`, its stream on `0xBF`.
  - The streams sit at the top of the command group range until F5b and F6 remove them.
- **Versions:** motor_manager and the IMU are at v6, time_sync at v2. v6 changed no payload: it came with the F2 move of the stream channels, and v5 with the F1b framing.
- **motor_manager and IMU configure commands:**
  - Commands and responses carry a host-chosen `seq` byte right after the version, echoed in the response.
  - Every response carries `result` and then a `detail` byte (the manager's `ResultDetail`) saying why it failed. The manager methods return the pair as a `Result`.
  - A command that arrives while the config queue is full is answered from the RX callback (`QueueFull`) instead of being dropped.
  - A response to a header that could not be parsed uses `0xFF` (`kUnparsedHeaderByte`) for the subcommand (and the motor id).
  - Configure bodies must have their exact size.
- **motor_manager:** `SET_TIMING` (`0x0B`) sets the control and telemetry periods. Its response appends the applied values after the 6-byte header.
- **IMU:** `SetStreamTiming` (`0x02`) sets the sample and telemetry periods. Its response appends the applied values after the 5-byte header.
- **Timestamps:** motor and IMU stream timestamps are wall-clock µs (`gettimeofday`, corrected by time_sync), latched at the sampling instant.
- **time_sync:** its ESP-initiated request carries a `u16 request_id`.

## Motor control loop

- A periodic `esp_timer` (task dispatch) wakes the motor_manager update task, which is pinned to core 1 at priority 20. The loop does not depend on `CONFIG_FREERTOS_HZ`.
- Each tick passes `dt = elapsed ticks × period` to `MotorController::update(dt)` and on to the PIDs. Nothing stores a sample time.
- The tick only try-locks the manager mutex. A busy mutex skips the tick (counted as an overrun) rather than blocking it. Keep the work done under `m_mutex` short.
- Shutdown stops the timer and waits out an in-flight callback before stopping the task.
- The control tick only counts a stalled timer. The motor config task watches the count and reports it.

## IMU sampling loop

- A `TaskTimer` wakes the IMU stream task every sample period. A packet is sent every N samples, with N derived from the telemetry period. The loop does not depend on `CONFIG_FREERTOS_HZ`.
- The tick reads the stream settings from atomics and only try-locks the `Bno055Device` mutex. A busy device skips the tick and counts it.
- Late, busy, read-error and encode-error counts are diag counters (`DIAG_COUNT`). Nothing is logged per tick.
- The timer is stopped while the stream is disabled and during an operation-mode switch.

## IMU vendor code and naming

- `firmware/components/imu_bmo_055/vendor/bno055.c` / `bno055.h` are the Bosch driver. Don't edit them. `vendor/` is a private include dir; only `Bno055Device` uses it.
- The component and namespace are spelled `imu_bmo_055`, but the part is the BNO055.
