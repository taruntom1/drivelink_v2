# Motor modules

F5a-1 supplies pure C++ motor interfaces, shared encoder math and
`controller.cascade_pid` (0x20). F5a-2 supplies the PWM+DIR actuator and PCNT
quadrature feedback implementations. No task or Kconfig tunable is introduced:
runtime settings and defaults are generated from YAML.

## Wiring and ownership

Wire a controller's required `actuator` and `feedback` refs in the same
real-time loop. Creation is dependencies first; ticks are feedback, controller,
actuator. `Instance::queryInterface` resolves `kActuatorInterface` and
`kFeedbackInterface` once at startup, without RTTI. Unknown interfaces return
null. The core remains independent of motor-specific interfaces.

An actuator exposes one loop-owned `ControlPort`. The controller claims it at
startup; a second claim fails. A shared lifetime keeps release safe regardless
of layout record destruction order. Allocate/copy shared ownership only at
startup/destruction; ticks access plain fields. A failed controller releases
its claim. Startup failures use `core.InstanceStartFailed`, with stage 1 for
missing/incompatible refs and 2 for an occupied actuator; both use cause -1.
The existing failed-instance ARM interlock applies.

The actuator implementation gates the port's duty with its independent
emergency force. The controller cannot clear that force. Actuators must retain
the core's `onArmTransition`/`emergencyStop` concurrency contract. A simulation
may additionally expose `plantObservation`; hardware actuators return false
and obtain physical feedback from a separate feedback module.

## Controller behavior

- `open_loop`: bounded `duty_sp` to duty.
- `velocity`: bounded `velocity_sp` through the velocity PID to duty.
- `position`: position PID through `velocity_limit`, then velocity PID through
  `duty_limit`. This is a true cascade, unlike the legacy position-to-duty PID.
- `vel.*`/`pos.*` gains and derivative cutoffs are live persisted parameters.
  Gains default to zero, mode to open-loop, velocity limit to 100 rad/s and
  duty limit to 1. The duty interval is symmetric and includes zero.
- Signals expose feedback, commanded duty, internal velocity target, waiting
  state and both PIDs' error/P/I/D/integrator values. I term and integrator are
  the same output-domain state. `reset_integrators` resets both PID histories.

Mode changes apply through the existing loop param batch. A repeated write of
the same mode does not create a new boundary. On a change while armed, capture
current feedback, hold the previous duty for that tick (within current limits),
and preset both loops' integrators without derivative kick. Until a fresh
active-input command arrives, hold measured velocity, measured position or
previous duty as appropriate. The setpoint is captured once, not continually
followed. P-only tuning can retain a transfer bias after an integrator preset;
nonzero integral tuning removes steady-state error.

Only a value-bearing command received strictly after mode application releases
`waiting_for_command`. Equal timestamps, old queued/inactive inputs, keepalives,
unrelated inputs and rejected frames do not. A fresh frame arriving during the
transition tick takes effect on the next tick. Receipt metadata is internal to
the owning loop; the wire payload is unchanged. The existing watchdog continues
to apply, including while waiting. Keepalives feed it but never release a mode's
waiting state.

ARM/DISARM clear histories/receipts and initialize duty/velocity to zero and
position to the current measurement. They never clear encoder position or
source storage. Disarmed mode changes cannot restore old duty on ARM. The
actuator force always takes precedence over commanded duty; the duty signal
is a controller command, not proof of physical effort.

The PID kernel is allocation-free and nonlogging, with derivative on
measurement and conditional integration at saturation. Shared derivative/output
math also serves the legacy PID wrapper, which retains its old integration
policy. Encoder count differencing handles int32 wrap. The legacy estimator
header forwards to this component until F5b removes the old controller.

## Validation

`test_motor_modules.cpp` covers control, six directed mode changes, receipts,
ownership, delays, emergency force, count wrap and repeatable noise with the
native real-time guard. Host integration runs `host/test/motor_smoke.py` through
the CLI on vdev. On the bare board use the same script with
`--endpoint serial://COM3 --loop-rate 1000`; real-module selection is reserved
for F5a-2. Wire items are documented under `docs/protocol/types/`.
