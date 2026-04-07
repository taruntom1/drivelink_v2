# motor_hal — Architecture & Internals

## High-Level Design
`motor_hal` separates public motor control from scarce MCPWM resource management. `Motor` is a thin owner of one allocated channel; `McpwmPool` manages global timer/operator/channel lifecycle with lazy allocation and teardown.

## Module Structure
| File | Responsibility |
|------|---------------|
| `include/esp_motor.hpp` | Public motor API |
| `priv_include/mcpwm_pool.hpp` | Internal shared MCPWM allocator interface |
| `src/esp_motor.cpp` | `Motor` implementation |
| `src/mcpwm_pool.cpp` | MCPWM pool allocation/deallocation logic |
| `Kconfig` | PWM frequency and timer resolution defaults |
| `CMakeLists.txt` | Component registration and dependencies |

## Key Data Structures
- `detail::McpwmChannel`: concrete handles for timer/operator/comparator/generator and slot identity.
- `McpwmPool::GroupState`: per-MCPWM-group timer and operator ownership counts.
- `Motor::Impl`: per-instance runtime state (`channel`, direction pin, cached speed, validity).

## Internal Call Flow
- `Motor` constructor -> configure DIR GPIO -> `McpwmPool::allocate()` -> mark valid.
- `setSpeed()` -> clamp input -> set DIR GPIO -> set comparator ticks.
- Destructor -> `stop()` -> `McpwmPool::deallocate()`.

## Memory Usage
- One heap allocation per `Motor` (`new Impl`).
- Static pool state (`s_groups`, `s_mutex`) is process lifetime.

## Hardware Interaction
- MCPWM timers/operators/comparators/generators.
- GPIO output for direction pin.
- PWM pin bound during generator creation.

## Initialization Sequence
1. Create direction GPIO output.
2. Lock pool and find free slot.
3. Ensure group timer exists and started.
4. Ensure operator exists and connected.
5. Create comparator + generator.
6. Program generator actions and return channel metadata.

## Kconfig Influence
- `CONFIG_MOTOR_HAL_PWM_FREQ_HZ`: sets period calculation and switching frequency.
- `CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ`: sets timer resolution and duty granularity.

## Extension Points
- Add additional drive modes (e.g., brake/coast) by extending `Motor` behavior.
- Add advanced waveform policies by enhancing generator action setup in pool allocation.
