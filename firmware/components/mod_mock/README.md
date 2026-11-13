# mod_mock

Pure deterministic module types for native tests, the F3c virtual device and
bare-board firmware checks. `mod_mock::kTypes` is the explicit registration
list. It uses the pure motor interfaces and estimator; allocation is at instance creation.

- actuator.mock (0x70, control): exact first-order velocity response to duty,
  with integrated position; configurable time constant and gain. `drive`
  supplies duty and samples velocity/position; paused `reset` zeros state
  and echoes a blob, exercising bounded bidirectional action payloads.
- feedback.mock (0x71, control): optional structural actuator reference,
  sampling its previous tick's state before the actuator ticks through
  optional simulation observations; unsupported actuator types fail
  safely at start.
- imu.mock (0x72, sensor): constant-rate yaw, wrapped to +/-pi, with sample
  and paused reset actions.
- fusion.mock (0x73, any loop): sources `feedback` and `imu`, their generated
  value/age signals, and `combined`, the sum of valid source values (missing
  values contribute zero). It exercises same-loop and cross-loop sampling
  without an actuator output. Source targets use packed signal/input refs.

Descriptors, IDs, params, signals, inputs and actions are generated from the
four module YAML files. Actions and ticks never allocate, block or log.
Native command tests and `smoke_core_firmware.py` exercise them. Command inputs
and arming integrate in F4; F3b's drive action is a simulation test control.


F4b gates actuator motion on an atomic emergency force, released by its owning
loop's arm transition. Disarmed drive actions leave zero target duty. The
`diagnostic(disarms, active)` loop action raises/clears either a mock arm blocker
or a disarming latched fault, allowing the real safety path to be tested without
motors. It is ordinary descriptor-driven behavior in the host.

## F5a-1 motor model

The actuator has `deadband` (default 0.02), symmetric `duty_limit` (1) and
`one_tick_delay` (true). The delay is in addition to feedback sampling the
preceding actuator state. The current cap constrains queued duty too. Emergency
force discards delayed effort, while the first-order plant can coast. ARM clears
the queued duty. `applied_duty` reports the plant's effort, distinct from a
controller's commanded duty.

`feedback.mock` quantizes simulated position to signed int32 `count` with
`counts_per_rev` (1440) and `gear_ratio` (1), then derives position and filtered
velocity from that count using the same estimator as hardware feedback.
The count wraps; the velocity delta handles wrap. Position is float radians
from the signed count and therefore wraps too. Use raw counts for odometry.
`velocity_cutoff` defaults to 30 Hz. `noise_counts` adds bounded count noise
before rounding (default off); `noise_seed` defaults to 1. The xorshift sequence
is deterministic. Plant reset clears the estimator and restarts the sequence;
ARM/DISARM re-prime the estimator without clearing encoder position.

Both mocks implement `mod_motor` interfaces. A controller claims its actuator
at startup, making the standalone duty input ineffective; `drive` then returns
Busy/AlreadyBound. Duplicate controllers fail startup and block ARM. A standalone
mock retains its original input/action API. There are no new wire encodings.

F6 extends `imu.mock` with representative quaternion/vector/temperature and
calibration-status signals, validity/age/count, modes and the shared versioned
calibration codec. `calibration_read`/`write` stage params through the paused
action transaction; `apply` updates the simulated sensor. Saved calibration is
applied at boot. Existing rate/yaw/sample/reset IDs remain stable. See
`docs/protocol/imu.md`; physical calibration quality is a separate HW check.
