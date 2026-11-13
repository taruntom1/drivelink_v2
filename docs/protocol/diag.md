# Diagnostic protocol, wire version 3

The redesign called "diag v2" uses version 3 (F1b used version 2 for framing).
Queries remain MSG_TYPE 0x23, responses 0x63, events 0xC0. All multi-byte fields
are little-endian. Timestamps are monotonic microseconds from boot; events
and info have reserved flags 0. Host time synchronization does not affect diag.

Event prefix: `version u8 | sequence u16 | kind u8 | flags u8`.

| Kind | Body |
|---|---|
| Record 1 | timestamp u64, severity u8, source u8, instance u8, code u16, esp_err i32, arg0 u32, arg1 u32 (25 bytes) |
| CounterDelta 2 | timestamp u64, count u8, entries: source u8, instance u8, code u16, delta u32, total u32 |
| Cleared 3 | timestamp u64, source u8, instance u8, code u16 |
| Boot 4 | timestamp u64, reset_reason u8, instances_ok u8, instances_failed u8, safe_mode u8 |

Instance 0xFF means no instance. Source/code definitions are in diag-codes.md;
module source IDs match module type IDs. The two args retain their documented
per-code meanings. Module InitFailed uses arg0 stage, arg1 unused, with the
instance in its own field. A removed manager's source id stays reserved and its
codes are retired (motor_manager 0x10 since F5b); never reuse them.

Query: `version u8 | seq u8 | op u8 | body`.
Response: `version u8 | seq u8 | op u8 | result u8 | body`.
Results: Ok 0, InvalidArgument 1, NotSupported 2. An unparsed op is 0xFF.

- GetInfo (1), empty request: flags u8, uptime_us u64, reset_reason u8,
  instances_ok u8, instances_failed u8, safe_mode u8, records_dropped u32,
  events_tx_dropped u32, version_length u8, firmware version bytes (max 32).
  Identity and descriptors belong to core system.GET_INFO.
- GetEntries (2), start slot u8: total u8, next u8, count u8, entries:
  source u8, instance u8, code u16, state u8, count u32. Next 0xFF means done.
  State bits: occurred 1, active 2. Empty slots are skipped.
- Clear (3), what u8: bit 1 clears occurred, bit 2 zeros counts. Active
  conditions remain active and instance slot identities remain stable.

There are 254 fixed slots. Non-instance slots retain their generated indexes;
instance identities claim remaining slots atomically, without allocation or
blocking. Slots persist until reboot. Exhaustion is counted as RecordDropped.
Records and clear events retry while the TX pool is full; queries can be
lost and are counted. Code metadata is served by describe.GET_CODES and is
covered by the descriptor hash.

F4b adds diagnostic code kind Latched (2) alongside Record (0) and Counter
(1), as served by core GET_CODES. Latched codes have an active bit and no
args. DIAG_RAISE and recovery update atomics and dirty bits; the reporter
sends ordinary Record/Cleared events with zero args and its observation time.
Repeated occurrences may coalesce, retaining both raise and clear when they
happen before a scan. Counts and current state remain queryable. No diag
packet format changed, so diag remains v3.

Every occurrence of a `disarms` code synchronously notifies the arm interlock,
independently of event queue or TX availability. It latches Fault except for
WatchdogTimeout. Active `blocks_arm` or `disarms` conditions refuse ARM;
active disarming conditions also refuse ACK_FAULT. Clear queries never clear
active conditions or acknowledge Fault. If report slots are exhausted, fixed overflow bitmaps still track each
safety code/instance until its condition clears. Reporting loss never drops
the arm/ACK interlock, and recovery does not require a reboot.
