# mod_mock

Pure deterministic module types for native tests, the F3c virtual device and
bare-board firmware checks. `mod_mock::kTypes` is the explicit registration
list. No HAL or OS dependency beyond core; allocation is at instance creation.

- actuator.mock (0x70, control): exact first-order velocity response to duty,
  with integrated position; configurable time constant and gain. `drive`
  supplies duty and samples velocity/position; paused `reset` zeros state
  and echoes a blob, exercising bounded bidirectional action payloads.
- feedback.mock (0x71, control): optional structural actuator reference,
  sampling its previous tick's state before the actuator ticks. F3b accepts
  actuator.mock wiring only; unsupported actuator types fail safely at start.
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
