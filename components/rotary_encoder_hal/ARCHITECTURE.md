# rotary_encoder_hal — Architecture & Internals

## High-Level Design
The component separates public encoder usage (`Encoder`) from pooled PCNT hardware management (`PcntPool`). `Encoder` stores rate-sampling state while `PcntPool` owns heavy peripheral configuration.

## Module Structure
| File | Responsibility |
|------|---------------|
| `include/encoder.hpp` | Public encoder API |
| `priv_include/pcnt_pool.hpp` | Internal PCNT pool interface |
| `src/encoder.cpp` | Rate/direction logic and instance lifecycle |
| `src/pcnt_pool.cpp` | PCNT allocation/configuration/teardown |
| `Kconfig` | Filter, sample period, counter limits |
| `CMakeLists.txt` | Dependencies |

## Key Data Structures
- `PcntUnit`: handles for PCNT unit and both channels.
- `EncoderImpl`: per-instance state (`prev_count`, `prev_time_us`, `current_rate`, `last_delta`).

## Internal Call Flow
- Constructor -> `PcntPool::allocate()` -> read initial count -> mark valid.
- `getRate()/getDirection()` -> lock -> conditional resample -> return cached value.
- Destructor -> `PcntPool::deallocate()`.

## Memory Usage
- One heap allocation per `Encoder` (`EncoderImpl`).
- Static pool metadata for active-count and mutex.

## Hardware Interaction
- PCNT unit/channel creation, watch points, counter clear/start/stop.
- GPIOs attached as quadrature edge/level inputs.

## Initialization Sequence
1. Ensure static pool mutex.
2. Create PCNT unit with limits and accumulator enabled.
3. Optionally apply glitch filter.
4. Create/configure channel A and B actions.
5. Add high/low watch points.
6. Enable, clear, and start unit.

## Kconfig Influence
- Filter width, sample period, and counter limits directly affect runtime sampling and wrap behavior.

## Extension Points
- Add optional ISR/event-driven rate updates if lower-latency response is needed.
- Add configurable decode modes beyond current quadrature setup.
