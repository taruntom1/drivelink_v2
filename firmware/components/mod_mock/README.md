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

Descriptors, IDs, params, signals, inputs and actions are generated from the
three module YAML files. Actions and ticks never allocate, block or log.
Native command tests and `smoke_core_firmware.py` exercise them. Command inputs
and arming integrate in F4; F3b's drive action is a simulation test control.
