# Time protocol v1 (F8)

The host initiates pings on registry `time_request` (0x02); the device answers
on `time_response` (0x42). This protocol has its own version, independent of
core v11 and diag v3. All multi-byte fields are little-endian. The device never
sets a wall clock: every device timestamp is monotonic microseconds since boot.

| Payload | Fields | Exact size |
|---|---|---|
| Request | `version u8 = 1, id u16` | 3 bytes |
| Response | `version u8 = 1, id u16, esp_rx_us u64, esp_tx_us u64` | 19 bytes |

`id` is chosen by the host and echoed without alteration. A valid request is
answered directly in the UART RX callback, including while disarmed or in safe
mode, without entering the config queue. RX time is sampled at callback entry.
TX time is patched into the queued reply by the TX task immediately before
submission to the UART driver; the CRC16 is recomputed after the patch. Stream
and ordinary command frames never receive this patch.

Malformed requests (including extra bytes and unsupported versions) are dropped
and counted by `core.BadTimeRequest` (source 0x04, code 0x0106). This is an
explicit exception to the ordinary command result/detail response convention.
There is no error response, result/detail or sequence byte beyond the u16 ID.
If the preallocated TX pool is exhausted, the response is dropped and counted
by `core.ResponseDropped`; RX never waits for a free buffer. The host times out.

## Timestamp boundaries and limits

RX is a complete validated frame's software callback time, not its first wire
bit. TX is driver submission, not electrical transmission. Existing bytes in
the driver/FIFO, serial frame durations, USB scheduling, kernel buffering and
path asymmetry remain uncertainty. Pings do not drain or reconfigure the UART
or change its FIFO priority. vdev takes equivalent RX and write-initiation
timestamps, patching a queued reply before TCP or PTY writing begins.

The SDK timestamps its send on the I/O thread when the queued ping starts
writing, and its receive when the matching complete response is scanned. Caller
wakeup and time spent waiting in the application's TX queue are excluded.
Host timestamps use `steady_clock` microseconds, with no UTC interpretation.
On Windows, the host overrides Asio 1.30.2's inter-byte-gap read timeout with
Win32's first-byte return mode (`MAXDWORD`, `MAXDWORD`, idle constant 1000 ms).
Available bytes return immediately; the constant bounds only a no-data wait.
Write timeouts and DTR/RTS remain unchanged. This prevents continuous telemetry
from holding a reply until the 2048-byte input buffer fills. See the
[COMMTIMEOUTS special mode](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts).

Given host send/receive `h0,h3` and device receive/send `d1,d2`:

- Corrected round trip is `(h3-h0)-(d2-d1)`.
- The fitted observation pairs device midpoint `(d1+d2)/2` and host midpoint
  `(h0+h3)/2`; implementations compute differences first to avoid overflow.
- Fit `host = host_reference + slope*(device-device_reference)`.
  Reported drift is `(slope-1)*1e6` ppm: host microseconds per device microsecond.

Constant asymmetric delay can bias offset even when the residual is tiny.
Low RTT selection reduces variable queueing error; it cannot prove absolute
accuracy or a bound on one-way delay.

## Host estimator and lifecycle

`Session::timePing()` returns a typed `TimeSample`, does not retry automatically,
and shares the session's serialized synchronous request access. Replies must
match their u16 ID, exact length and version. Unrelated/stale replies are
ignored; matching bad versions/lengths are protocol errors. Timed-out and failed
IDs are retired until reconnect, and exhaustion requires an explicit reconnect.
Successful IDs may wrap. Close/transport errors wake the caller. Ping traffic
does not feed the device command watchdog.

Each sample carries a process-local clock generation, changed on a new Session
or a valid diagnostic Boot event. A ping spanning Boot is rejected. The pure
single-caller `ClockEstimator` resets on a new generation or backwards device
time, and rejects invalid timestamp order, negative corrected RTT and values
above the signed 64-bit clock domain. Its memory capacity is fixed at construction.

Defaults: retain a 120 s sliding window (capacity 2048 observations), choose the
lowest-RTT observation per 5 s host-time bin, and use centered linear least
squares. Offset-only warmup uses slope 1. Drift becomes valid with at least
eight selected observations spanning 30 s; models expire after 5 s without an
accepted sample. Options expose those limits. Model snapshots report validity,
reference times, slope/drift, selected count, span, age, RTT range and RMS fit
residual. Callers should evaluate age/quality before using a mapping.

`time-ping` prints one sample. `time-sync [DURATION_S]` samples for 180 s by
default (range 1–3600): approximately every 100 ms for five seconds, then every
second. Readable/JSON output includes raw samples, model snapshots, prior
held-out predictions, timeouts and event-queue drops. CLI sessions use this
explicit cadence. JSON additionally reports the production Session mapping,
generation, validity and uncertainty; readable output is unchanged.

H1b owns periodic pings on the library I/O thread (100 ms initial delay, 200 ms
cadence for three seconds, then 1 s). Boot resets its fit. Timestamped status,
diagnostics, subscriptions and capture results retain device time and a mapped
host time with generation/validity/uncertainty. Frame/reply metadata is stamped
on receive; a queued request sent after Boot receives the new generation.
Uncertainty includes RTT, fit residual/extrapolation and age, not an absolute
bound on path asymmetry. Invalid or stale models must not be used as valid
timestamps. No wire payload changes. H3 distributes the agent's clock model.

`host/test/time_smoke.py` measures p95 held-out low-RTT midpoint residual and
successive-model prediction changes at the same device time. Its CH343 defaults
are at least 95% successful pings, a mature model, residual <=2 ms and prediction
change <=2 ms. The Pi target is a 100 us residual threshold; neither check is an
absolute offset-accuracy test. See the F8 plan/results and deferred hardware table.

The legacy device-initiated `time_sync` 0x20/0x60 protocol, periodic task and
wall-clock setter are removed. Legacy diagnostic source/code numbers remain
reserved as retired entries; they are not reused for this protocol.
