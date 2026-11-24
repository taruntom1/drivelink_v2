# Motor modules

The motor module types, the pure motor interfaces ([`interfaces.hpp`](include/mod_motor/interfaces.hpp)) and the shared [velocity estimator](include/mod_motor/velocity_estimator.hpp). Runtime settings come from the type YAML; the MCPWM timer resolution and PCNT limits are Kconfig in [motor_hal](../motor_hal/README.md) and [rotary_encoder_hal](../rotary_encoder_hal/README.md).

| Type | Generated reference | Definition | Registered by |
|---|---|---|---|
| `controller.cascade_pid` | [controller.cascade_pid.md](../../../docs/protocol/types/controller.cascade_pid.md) | [YAML](../../../protocol/defs/controller.cascade_pid.yaml) | `kTypes`: `main` and vdev |
| `actuator.pwm_dir` | [actuator.pwm_dir.md](../../../docs/protocol/types/actuator.pwm_dir.md) | [YAML](../../../protocol/defs/actuator.pwm_dir.yaml) | `kHardwareTypes`: `main` only |
| `feedback.quadrature` | [feedback.quadrature.md](../../../docs/protocol/types/feedback.quadrature.md) | [YAML](../../../protocol/defs/feedback.quadrature.yaml) | `kHardwareTypes`: `main` only |

Diagnostic codes, stages and arguments are in [diag codes](../../../docs/protocol/diag-codes.md).

## Hardware types

The module logic ([`pwm_dir.cpp`](src/pwm_dir.cpp), [`quadrature.cpp`](src/quadrature.cpp)) is pure. It reaches hardware only through [`hardware.hpp`](include/mod_motor/hardware.hpp): `PwmDirOutput` (`write`, `forceLow`, `releaseForce`) and `QuadratureCounter` (`read`), opened at `start()`. The device backend is [`src/idf/motor_hw_idf.cpp`](src/idf/motor_hw_idf.cpp) over `motor_hal::Motor` and `rotary_encoder_hal::Encoder`; native tests link [`fake_motor_hw.cpp`](../../test/native/tests/fake_motor_hw.cpp).

`start()` binds everything first and takes the hardware last: an instance that failed after taking it would hold an output no monitor list stops.

### actuator.pwm_dir

It has no duty input of its own; only a claimed controller port drives it. `duty_limit` is a hard cap after the controller. `applied_duty` is reported after the gate and cap, before `invert`. `invert` is locked while armed.

- Each instance takes an **MCPWM operator of its own** (six on the ESP32-S3). An operator's two generators share one force register that the driver updates read-modify-write, so outputs forced and released from different tasks must not share it. `motor_hal` gives every motor its own operator.
- Each instance also takes an **MCPWM timer of its own** for `pwm_freq_hz` (5 to 100 kHz, default 25 kHz, applied at the next boot). A higher frequency leaves fewer duty steps; see [motor_hal](../motor_hal/README.md#kconfig).
- The output is **forced low from creation**, before the PWM actions are set, until `onArmTransition(true)`. That transition writes zero duty, releases the force and only then opens the atomic gate. DISARM and `emergencyStop()` force the hardware and close the gate.
- Every stop first bumps an atomic stop epoch. The ARM release reads the epoch before and after and stops again if a monitor stop raced it, so a stale release never undoes a monitor stop. The monitor's force also repeats while disarmed.
- Only the PWM pin is forced; DIR is left as it is. DIR is high for negative duty (before `invert`) and is written only with a nonzero compare value (`Motor::write`); a zero write leaves it.
- **A reversal passes through one zero tick.** A nonzero duty is written only when the previous request had the same sign or was zero. The compare value applies at the next PWM period but DIR at once, so DIR never flips under a live compare value of the other sign. That needs the zero write and the next one to be more than a PWM period apart. The `pwm_freq_hz` range guarantees a PWM period (at most 200 µs) below the shortest loop period (500 µs at 2 kHz), and a native test ties the two ranges together. A tick that starts late can still come closer than that to the next one: at both limits, if it is over 300 µs late, or 960 µs late with the default 25 kHz at 1 kHz. A request that alternates sign every tick writes zero rather than a one-sided bias.

### feedback.quadrature

Each instance takes one PCNT unit (four on the ESP32-S3). `glitch_ns` (0 to 10000, default 1000, 0 turns it off; applied at the next boot) sets its glitch filter. `pullup` selects the PCNT driver's own pull-ups. Signal IDs match `feedback.mock`. The module never resets the count; arm transitions re-prime only the velocity estimator. Changing `invert`, which is locked while armed, flips the count's sign and re-primes the estimator. IDF 5.5 compensates an overflow interrupt still pending at a read.

### Failures

- **Start:** each hardware type reports its own `InitFailed` in place of `core.InstanceStartFailed`, with a stage for an unset pin or a driver failure (`ESP_ERR_NOT_FOUND` when operators or units are used up). The failed-instance ARM interlock applies.
- **Tick:** `actuator_pwm_dir.DriverFault` and `feedback_quadrature.ReadFailed` are latched codes flagged `disarms`, raised on the first faulty tick or by the monitor when its force fails.
- A failed write or release forces the output. DriverFault clears on the next tick whose write succeeds and whose last force succeeded, whichever side raised it; a failed release therefore clears at once, so ARM can be retried after ACK_FAULT.
- A failed read keeps the count and reports NaN position and velocity, so the controller outputs zero duty that tick. ReadFailed clears on the next good read.

## Wiring and ownership

Wire a controller's required `actuator` and `feedback` refs in the same real-time loop. Creation is dependencies first; ticks run feedback, controller, actuator. `Instance::queryInterface` resolves `kActuatorInterface` and `kFeedbackInterface` once at startup, without RTTI; unknown interfaces return null. The core stays independent of motor-specific interfaces.

An actuator exposes one loop-owned `ControlPort`. The controller claims it at startup and a second claim fails. A shared lifetime keeps release safe regardless of layout record destruction order. Allocate or copy shared ownership only at startup and destruction; ticks access plain fields. A failed controller releases its claim. Controller startup failures use `core.InstanceStartFailed` with a stage for missing or incompatible refs or an occupied actuator, and the failed-instance ARM interlock applies.

The actuator gates the port's duty with its independent emergency force, which the controller cannot clear. Actuators must keep the core's `onArmTransition`/`emergencyStop` concurrency contract. A simulation may also expose `plantObservation`; hardware actuators return false and get physical feedback from a separate feedback module.

## Controller

- `open_loop`: bounded `duty_sp` to duty.
- `velocity`: bounded `velocity_sp` through the velocity PID to duty.
- `position`: position PID through `velocity_limit`, then velocity PID through `duty_limit`. Position feeds velocity, velocity feeds duty.
- `vel.*` and `pos.*` gains and derivative cutoffs are live persisted parameters. The duty interval is symmetric and includes zero.
- Signals expose feedback, commanded duty, the internal velocity target, the waiting state and both PIDs' error, P, I, D and integrator values. The I term and integrator are the same output-domain state. `reset_integrators` resets both PID histories.

The observable mode-change and `waiting_for_command` rules are in [Motor controller commands](../../../docs/protocol/core.md#motor-controller-commands-f5a-1). Implementation details:

- Mode changes apply through the loop param batch. On a change while armed, the controller captures current feedback, holds the previous duty for that tick within current limits, and presets both integrators without derivative kick.
- Until a fresh active-input command arrives it holds measured velocity, measured position or previous duty as the mode requires. The setpoint is captured once, not continually followed. P-only tuning can retain a transfer bias after an integrator preset; nonzero integral gain removes steady-state error.
- Inactive or unrelated inputs and rejected frames never release `waiting_for_command`. Receipt metadata is internal to the owning loop; the wire payload is unchanged.
- ARM and DISARM clear histories and receipts and initialize duty and velocity to zero and position to the current measurement. They never clear encoder position or source storage, and disarmed mode changes cannot restore old duty on ARM.
- The actuator force always takes precedence; the duty signal is a controller command, not proof of physical effort.

The PID kernel is allocation-free and nonlogging, with derivative on measurement and conditional integration at saturation. Encoder count differencing handles int32 wrap.

## Validation

`test_motor_modules.cpp` covers control, six directed mode changes, receipts, ownership, delays, emergency force, count wrap and repeatable noise under the native real-time guard. `test_motor_hardware.cpp` drives the hardware types through the fake backend: forced from creation, the arm gate, cap, invert, the reversal tick and sign alternation, a monitor stop inside the ARM release, monitor stops concurrent with ticks, DriverFault and ReadFailed, start stages, destruction, the PWM frequency and glitch filter from settings to driver, count scaling, invert and wrap, and velocity and position control on a fake plant.

Host integration runs `host/test/motor_smoke.py` through the CLI on vdev. For bare-board runs, real-module pins and first-run safety, see the [motor controller smoke](../../../host/README.md#motor-controller-smoke-f5a-1) and the [bench instructions](../../../docs/bench.md#pins-for-real-modules).
