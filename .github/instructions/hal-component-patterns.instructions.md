---
description: "Use when creating or refactoring ESP-IDF HAL components under components/. Enforces motor_hal-style structure, ownership, error handling, and resource management patterns."
applyTo: "components/**"
---

# HAL Component Patterns

## Structure
- Use include/ for public headers only.
- Use priv_include/ for internal headers only.
- Use src/ for implementation.
- Add Kconfig for tunable defaults and valid ranges.
- Use ESP-IDF component CMake with explicit SRCS, INCLUDE_DIRS, PRIV_INCLUDE_DIRS, and REQUIRES.

## Public API Design
- Keep API small and hardware-focused.
- Hide low-level driver state in internal structs/classes.
- For hardware-owning types, delete copy constructor/assignment.
- Allow move semantics only when ownership transfer is safe.
- Provide an isValid()-style readiness check when construction can fail.

## Error Handling and Logging
- Check every ESP-IDF return value.
- Log failures with esp_err_to_name.
- Keep one module-level TAG per source file.
- Include actionable context in logs (GPIOs, channels, slot IDs, config values).
- Fail early and preserve a consistent object state after failures.

## Resource Management
- Centralize scarce peripheral allocation in a pool/manager.
- Enforce hard resource limits in allocation paths.
- Release resources deterministically in destructors.
- Guard shared pool mutations with FreeRTOS mutex protection.

## API Behavior
- Clamp user-facing numeric inputs at API boundaries.
- Route convenience APIs through one canonical implementation path.
- Keep stop/reset operations explicit and idempotent.
- Assume same-instance concurrent writes require external synchronization unless explicitly designed otherwise.

## Validation Expectations
- Build with idf.py build after component changes.
- If config/target behavior is inconsistent, run idf.py fullclean then rebuild.
- Since there is no unit test suite, validate behavior through logs and runtime checks.