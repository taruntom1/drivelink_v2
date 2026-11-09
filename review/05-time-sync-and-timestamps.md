# 05: Time Sync and Timestamps

---

## T1. Medium: telemetry streams use different time bases, and the docs disagree with the code

| Stream | Code | Time base |
|--------|------|-----------|
| Motor telemetry (`0x81`) | [motor_manager.cpp:593-596](../components/motor_manager/src/motor_manager.cpp) `gettimeofday()` | **Wall clock (Unix µs)**, stepped by time_sync every 5 s |
| IMU stream (`0x82`) | [bno055_device.cpp:175](../components/imu_bmo_055/src/bno055_device.cpp) / [imu_bmo_055.cpp:221](../components/imu_bmo_055/src/imu_bmo_055.cpp) `esp_timer_get_time()` | **Monotonic µs since boot** |
| Motor README | [README.md:196](../components/motor_manager/README.md) | Says "microseconds since boot" |

Consequences:
- The host cannot line up IMU samples with motor samples without knowing the current offset, which it never receives.
- Before the first sync, motor timestamps are ~0 (1970). After the first sync they jump to ~1.7e15. A host decoder that follows the README breaks.
- Every periodic sync calls `settimeofday()` and **steps** the clock, so motor timestamps can go backwards or jump forwards by the drift accumulated over 5 s.

**Status: partly fixed.**
- The README drift is fixed. The motor_manager README now says `timestamp_us` comes from `gettimeofday()` (host-synced once TimeSync has run).
- The motor timestamp is now latched in the control tick that sampled the motors (`wallClockUs()` in `motor_manager.cpp`).
- **Fixed** (IMU protocol v3): the IMU stream now also uses `gettimeofday()` wall-clock time, taken at the midpoint of the sample's I²C reads, so both streams share one time base.
- Still open: the clock is still stepped (T2).

**Recommendation**: pick one base for all streams. The usual choice is the synced Unix time, because that is what time_sync exists for. Build it as `esp_timer_get_time() + offset` that is *slewed*, not stepped (see T2). Expose one `time_sync::nowUs()` helper and use it from both managers. Update both READMEs.

---

## T2. Medium: `settimeofday()` steps the clock on every sync

[time_sync.cpp:427-428](../components/time_sync/src/time_sync.cpp)

Each successful sync overwrites the system time. Besides the telemetry jumps in T1, this:
- applies RTT noise directly to the clock. With a 921 kbps UART and USB-serial latency, RTT jitter on the PC side is commonly 1–10 ms, so the clock jitters by about half that every 5 s.
- has no outlier rejection. One delayed response (USB scheduling hiccup) moves the clock by milliseconds.

**Fix**: keep `offset_us` as state, filter it (min-RTT selection over N samples, or an EMA), and apply it to timestamps (or use `adjtime()`) instead of stepping. Reject samples whose RTT exceeds, for example, 2× the recent minimum.

---

## T3. Low: algorithm details

- The non-compensated branch ([time_sync.cpp:414](../components/time_sync/src/time_sync.cpp)) uses `offset = t2 - t3`. That ignores the return-path delay entirely, so it is biased by the full one-way delay. A better fallback without server rx/tx timestamps is `t2 - (t0 + t3)/2`.
- `corrected_now_us = t3 + offset` is applied a few µs to ms after `t3`: logging, mutex, and other tasks preempting at priority 6. Recompute "now" right before `settimeofday`: `nowUnixUs() + offset`.
- `TimeSync` starts the periodic task inside its constructor when `sync_interval_ms > 0` ([time_sync.cpp:134-139](../components/time_sync/src/time_sync.cpp)), and `main.cpp` calls `startPeriodicSync()` again ([main.cpp:58](../main/main.cpp)). The second call is a no-op. Remove one of them; the explicit call in `main` is clearer.
- Task stack (4096) and priority (6) are hard-coded ([time_sync.cpp:219-225](../components/time_sync/src/time_sync.cpp)), unlike every other component, which uses Kconfig.
- Shutdown: see C2 in [01](01-concurrency-and-lifecycle.md). The periodic task sleeps 5 s, so `stopPeriodicSync()` always force-deletes it. **Fixed** by C1/C2: the task is a `StoppableTask`, and `sleepUntil` wakes as soon as a stop is requested.
