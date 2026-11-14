# rotary_encoder_hal

A move-only quadrature encoder counter on the ESP32-S3 PCNT. It only counts;
`feedback.quadrature` (`mod_motor`) turns counts into position and velocity
(`VelocityEstimator`, sampled at the loop rate).

## API (`encoder.hpp`)

| Call | Use |
|---|---|
| `Encoder(pin_a, pin_b, options)`, `isValid()`, `initError()` | Takes one PCNT unit (both channels, 4x decoding) from `PcntPool` and applies the glitch filter. |
| `readCount(count)` | The accumulated signed count, extended past the 16-bit hardware counter by overflow watch points. |

## Resources

The ESP32-S3 has 4 PCNT units, so at most 4 encoders run at once. The pool is
guarded by a FreeRTOS mutex and enforces the limit in the allocation path.

## Kconfig ("Rotary Encoder HAL Configuration")

`ENCODER_HAL_GLITCH_FILTER_NS` (default 1000 ns, 0 disables) and the PCNT
high/low limits (default the full 16-bit range, so overflows are rare).

## Rules

- It returns `esp_err_t`/`bool` and never reports through `diag`.
- Not run on a real encoder yet: see "Deferred hardware checks" in
  `plans/host-status.md`.
