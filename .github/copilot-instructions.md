# Project Guidelines

## Code Style
- Use C++ for HAL and app logic.
- Keep ESP-IDF logging and error handling explicit:
  - Use module-level TAG constants.
  - Check ESP-IDF return codes and log failures with esp_err_to_name.
- Respect ownership semantics for hardware-backed classes:
  - Do not copy motor objects; they are move-only by design.

## Component Pattern (Use for New HAL Components)
- Use this component layout:
  - include/: public headers only.
  - priv_include/: internal headers only.
  - src/: implementation files.
  - Kconfig: tunable defaults and ranges for timing/peripheral parameters.
  - CMakeLists.txt: idf_component_register with explicit SRCS, INCLUDE_DIRS, PRIV_INCLUDE_DIRS, and REQUIRES.
- Public API pattern:
  - Provide a small, hardware-focused class API in include/.
  - Hide low-level driver state in an internal Impl/pool/internal struct.
  - Make hardware-owning types non-copyable; allow move when ownership transfer is safe.
  - Expose an isValid()-style readiness check when initialization can fail at runtime.
- Error handling and logging pattern:
  - Validate every ESP-IDF call result and log failures with esp_err_to_name.
  - Keep logs actionable: include pin numbers, channel/operator IDs, and resource slot info.
  - Fail early on invalid state and keep object state consistent after failures.
- Resource management pattern:
  - Centralize scarce peripheral allocation in a pool/manager object.
  - Track ownership and release deterministically in destructors.
  - Protect shared pool mutation with a FreeRTOS mutex.
  - Document hard resource limits (for example max instances) and enforce them in allocation paths.
- API behavior pattern:
  - Clamp user-facing numeric ranges at the API boundary.
  - Provide convenience wrappers (for example percent-based setters) that map to a single canonical path.
  - Keep stop/reset operations explicit and idempotent.
  - Treat concurrent writes to the same instance as externally synchronized unless explicitly made thread-safe.

## Architecture
- This is an ESP-IDF firmware project for ESP32-S3.
- Entry point is main/main.cpp.
- Components are organized under components/:
  - components/motor_hal: production C++ HAL for PWM + DIR motor control.
  - components/rotary_encoder_hal: currently minimal/stub component.
- In components, keep public headers in include/ and internal-only headers in priv_include/.

## Build and Test
- Preferred local validation flow:
  - idf.py build
  - idf.py flash
  - idf.py monitor
- Combined flow: idf.py build flash monitor
- Configuration flow: idf.py menuconfig (Motor HAL Configuration menu contains motor timing settings).
- If target or low-level config changes cause inconsistent builds, run idf.py fullclean before rebuilding.
- There is currently no automated unit test suite in this repo; validate behavior via build success and runtime logs.

## Conventions
- Keep CMake component declarations in ESP-IDF style with idf_component_register and explicit REQUIRES dependencies.
- Preserve current resource model assumptions:
- Do not edit generated artifacts under build/.

## Key References
- main/main.cpp
- components/motor_hal/include/esp_motor.hpp
- components/motor_hal/src/esp_motor.cpp
- components/motor_hal/src/mcpwm_pool.cpp
- components/motor_hal/Kconfig
- components/motor_hal/CMakeLists.txt
- .vscode/settings.json