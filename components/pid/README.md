# ESP PID Controller

A lightweight, hardware-independent **PID controller implementation** designed for real-time control loops on ESP-IDF (or any C++ environment).

This module supports:

* Closed-loop PID control
* Configurable sampling time
* Output clamping
* Derivative filtering
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
* ✔ Configurable **sample time**
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
cfg.sample_time_ms = 10;
cfg.output_min = -1.0f;
cfg.output_max = 1.0f;
cfg.direction = esp_pid::Direction::Direct;
cfg.derivative_mode = esp_pid::DerivativeMode::OnMeasurement;
cfg.derivative_filter_alpha = 0.1f;
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

Call this **at a fixed interval equal to `sample_time_ms`**:

```cpp
float output = pid.compute(input, setpoint);
```

With feedforward:

```cpp
float output = pid.compute(input, setpoint, feedforward);
```

---

## Example

```cpp
esp_pid::PidConfig cfg;
cfg.kp = 2.0f;
cfg.ki = 1.0f;
cfg.kd = 0.2f;

esp_pid::PidController pid(cfg);
pid.setMode(esp_pid::Mode::Automatic);

while (true) {
    float measured = read_sensor();
    float target = 100.0f;

    float control = pid.compute(measured, target);

    apply_output(control);

    vTaskDelay(pdMS_TO_TICKS(cfg.sample_time_ms));
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

## Sample Time

```cpp
pid.setSampleTime(20); // ms
```

* Must match your control loop timing
* Internally rescales integral and derivative terms

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
float output = pid.compute(input, setpoint, ff);
```

---

## Best Practices

* Call `compute()` at a **fixed, consistent interval**
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