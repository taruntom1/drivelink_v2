# Motor modules

The motor module types: `controller.cascade_pid` (0x20, F5a-1),
`actuator.pwm_dir` (0x21) and `feedback.quadrature` (0x22, F5a-2), with the
pure motor interfaces and shared encoder math. No task or Kconfig tunable is
introduced here: runtime settings and defaults are generated from YAML. The PWM
frequency, the MCPWM timer resolution (default 10 MHz: 400 steps at 25 kHz)
and the PCNT glitch filter stay in the `motor_hal` / `rotary_encoder_hal`
Kconfig menus.

`kTypes` holds the pure controller, which the vdev also registers.
`kHardwareTypes` holds the two hardware types; `main` registers them, and the
vdev does not (it has no pins).

## Hardware types (F5a-2)

The module logic (`src/pwm_dir.cpp`, `src/quadrature.cpp`) is pure. It reaches
the hardware only through `mod_motor/hardware.hpp`: `PwmDirOutput`
(`write`, `forceLow`, `releaseForce`) and `QuadratureCounter` (`read`), opened
at `start()`. The device backend is `src/idf/motor_hw_idf.cpp` over
`motor_hal::Motor` and `rotary_encoder_hal::Encoder`; the native tests link a
fake (`firmware/test/native/tests/fake_motor_hw.cpp`).

**`actuator.pwm_dir`:** `pwm_pin`, `dir_pin`, `invert` (locked while armed) and
`duty_limit` (a hard cap after the controller); signals `applied_duty` (after
the gate and cap, before invert) and `forced`. It has no duty input of its own:
only a claimed controller port drives it.
- Each instance takes an **MCPWM operator of its own** (6 on the ESP32-S3). The
  two generators of an operator share one force register, which the driver
  updates read-modify-write, so outputs forced and released from different
  tasks must not share it. Legacy motors still share operators until F5b.
- The output is **forced low from creation** (before the PWM actions are set)
  until `onArmTransition(true)`, which writes zero duty, releases the force and
  only then opens the atomic gate. DISARM and `emergencyStop()` force the
  hardware and close the gate. Every stop first bumps an atomic stop epoch;
  the release reads it before and after, and stops again if a monitor stop
  raced it, so a stale release never undoes a monitor stop. The monitor's
  force also repeats while disarmed.
- The PWM pin is the only one forced; DIR is left as it is. DIR is high for
  negative duty (before `invert`) and is written only with a nonzero compare
  value (`Motor::write`): a zero write leaves it.
- **A reversal passes through one zero tick.** A nonzero duty is written only
  when the previous request had the same sign or was zero. The compare value
  applies at the next PWM period but DIR at once, so DIR never flips under a
  live compare value of the other sign, provided the loop period is longer
  than the PWM period (1 ms against 40 us by default). A request that
  alternates sign every tick writes zero rather than a one-sided bias.
- `start()` binds everything first and takes the hardware last: an instance
  that failed after taking it would hold an output no monitor list stops.

**`feedback.quadrature`:** `pin_a`, `pin_b`, `pullup` (default true: the PCNT
driver's own pull-ups; false disables them), `counts_per_rev`, `gear_ratio`,
`velocity_cutoff` and `invert` (locked while armed; a change flips the count's
sign and re-primes the estimator); signals `count`, `velocity`, `position`
(same ids as `feedback.mock`). One PCNT unit each (4). The module never resets
the count; arm transitions re-prime only the velocity estimator. IDF 5.5
compensates an overflow interrupt still pending at a read.

**Failures.**
- Start: the type's own `InitFailed` (code 1, which the core sends in place of
  `core.InstanceStartFailed`) with stage 1 for a pin left unset (cause -1) and
  2 for a driver failure with its `esp_err` (`ESP_ERR_NOT_FOUND` when the
  operators or units are used up). The failed-instance ARM interlock applies.
- Tick: `actuator_pwm_dir.DriverFault` and `feedback_quadrature.ReadFailed`
  are latched codes flagged `disarms`, raised on the first faulty tick (or by
  the monitor when its force fails). A failed write or release forces the
  output; DriverFault clears on the next tick whose write succeeds and whose
  last force succeeded, whichever side raised it (a failed release therefore
  clears at once, so ARM can be retried after ACK_FAULT). A failed read keeps
  the count and reports NaN position and velocity, so the controller zeroes
  its duty that tick; it clears on the next good read.

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
native real-time guard. `test_motor_hardware.cpp` drives the hardware types
through the fake backend: forced from creation, the arm gate, cap, invert,
the reversal tick and sign alternation, a monitor stop placed inside the ARM
release, monitor stops concurrent with ticks, DriverFault and
ReadFailed, start stages, destruction, count scaling/invert/wrap, and velocity
and position control on a fake plant. Host integration runs
`host/test/motor_smoke.py` through the CLI on vdev. On the bare board use the
same script with `--endpoint serial://COM3 --loop-rate 1000`, and
`--modules real --no-motor --pool-check` with pins for the hardware types; a
motor run drops `--no-motor` and needs `--confirm-first-run`. Wire items are
documented under `docs/protocol/types/`.
