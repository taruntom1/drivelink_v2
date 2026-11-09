# ESP PID Controller

A lightweight, hardware-independent **PID controller implementation** designed for real-time control loops on ESP-IDF (or any C++ environment).

This module supports:

* Closed-loop PID control
* Caller-supplied `dt` on every call (works with variable or changing loop rates)
* Output clamping
* Derivative filtering (cutoff in Hz)
* Feedforward input
* Runtime mode switching (Manual ↔ Automatic)

---

## Features

* ✔ Pure algorithm (no hardware dependency)
* ✔ Supports **Direct** and **Reverse** control
* ✔ Two derivative modes:

  * On measurement (default, avoids setpoint kick)
  * On error
* ✔ Built-in **anti-windup via output clamping**
* ✔ Explicit **`dt`** per call, so I and D stay correctly scaled if the loop is late or its period changes
* ✔ Optional **feedforward term**
* ✔ Safe transitions between modes

---

## File Structure

```
esp_pid/
├── include/
│   └── pid_controller.hpp
├── src/
│   └── pid_controller.cpp
└── README.md
```

---

## Basic Usage

### 1. Include the header

```cpp
#include "pid_controller.hpp"
```

---

### 2. Configure the controller

```cpp
esp_pid::PidConfig cfg;
cfg.kp = 1.0f;
cfg.ki = 0.5f;
cfg.kd = 0.1f;
```

Optional parameters:

```cpp
cfg.output_min = -1.0f;
cfg.output_max = 1.0f;
cfg.direction = esp_pid::Direction::Direct;
cfg.derivative_mode = esp_pid::DerivativeMode::OnMeasurement;
cfg.derivative_cutoff_hz = 50.0f; // 0 = unfiltered (Kconfig default)
```

---

### 3. Create the controller

```cpp
esp_pid::PidController pid(cfg);
```

---

### 4. Enable automatic mode

```cpp
pid.setMode(esp_pid::Mode::Automatic);
```

---

### 5. Run in control loop

Pass the time since the previous call, in seconds:

```cpp
float output = pid.compute(input, setpoint, dt_s);
```

With feedforward:

```cpp
float output = pid.compute(input, setpoint, dt_s, feedforward);
```

If `dt_s` is not positive (or not finite), the state is left untouched and the previous output is returned.

---

## Example

```cpp
esp_pid::PidConfig cfg;
cfg.kp = 2.0f;
cfg.ki = 1.0f;
cfg.kd = 0.2f;

esp_pid::PidController pid(cfg);
pid.setMode(esp_pid::Mode::Automatic);

int64_t last_us = esp_timer_get_time();
while (true) {
    vTaskDelay(pdMS_TO_TICKS(10));

    const int64_t now_us = esp_timer_get_time();
    const float dt_s = (now_us - last_us) * 1e-6f;
    last_us = now_us;

    float measured = read_sensor();
    float target = 100.0f;

    float control = pid.compute(measured, target, dt_s);

    apply_output(control);
}
```

---

## Modes

### Manual Mode

```cpp
pid.setMode(esp_pid::Mode::Manual);
```

* `compute()` returns `0`
* Controller state is preserved

---

### Automatic Mode

```cpp
pid.setMode(esp_pid::Mode::Automatic);
```

* PID computation is active
* Automatically calls `reset()` to prevent bumps

---

## Tuning

Update gains at runtime:

```cpp
pid.setTunings(kp, ki, kd);
```

Constraints:

* Gains must be **≥ 0**
* Invalid values are ignored

---

## Output Limits

```cpp
pid.setOutputLimits(min, max);
```

* Prevents actuator saturation
* Helps reduce **integral windup**

---

## Direction Control

```cpp
pid.setDirection(esp_pid::Direction::Reverse);
```

Use **Reverse** when:

* Increasing output decreases the process variable

---

## Timing and Derivative Filter

There is no stored sample time: `compute()` scales the integral and derivative terms by the `dt_s` you pass. For a fixed-rate loop, pass the period. If you skip cycles, pass the real elapsed time.

```cpp
pid.setDerivativeCutoff(30.0f); // Hz, 0 = unfiltered
```

* First-order low-pass on the derivative rate. The coefficient is computed from `dt_s` on each call, so the filter behaves the same at any loop rate.
* Negative or non-finite values are rejected.

---

## Reset Controller

```cpp
pid.reset();
```

Clears:

* Integral accumulator
* Derivative history

---

## Diagnostics

```cpp
float kp = pid.getKp();
float ki = pid.getKi();
float kd = pid.getKd();

float integral = pid.getIntegralTerm();
float error = pid.getLastError();
float output = pid.getLastOutput();
```

---

## Feedforward Usage

You can improve performance by adding a feedforward term:

```cpp
float ff = model_based_estimate(setpoint);
float output = pid.compute(input, setpoint, dt_s, ff);
```

---

## Best Practices

* Call `compute()` at a **fixed, consistent interval** and pass the matching `dt_s`
* Start with **Ki = 0, Kd = 0**, tune Kp first
* Add Ki to remove steady-state error
* Add Kd to reduce overshoot
* Use **OnMeasurement derivative** to avoid spikes

---

## Common Pitfalls

| Issue             | Cause                                              |
| ----------------- | -------------------------------------------------- |
| Oscillation       | Kp too high                                        |
| Slow response     | Kp too low                                         |
| Overshoot         | Ki too high                                        |
| Noise sensitivity | Kd too high                                        |
| Sudden jumps      | Switching modes without reset (handled internally) |

---

## Validity Check

```cpp
if (!pid.isValid()) {
    // Handle initialization error
}
```

---