# diag

Reports errors, warnings, counts and latched conditions to the host as codes
(diag wire v3, `docs/protocol/diag.md`), and optionally as console text. The
codes are defined in `protocol/defs/diag_codes.yaml`, generated into the code
table and listed in `docs/protocol/diag-codes.md`; call sites use
`diag::codes::<source>::k<Name>` through `diag_codes.hpp`.

## API

**Reporting (`diag.hpp`).** Use the macros, never the functions behind them:

| Macro | Context | Use |
|---|---|---|
| `DIAG_ERROR(code, err, arg0, arg1, fmt, ...)`, `DIAG_WARN(...)` | task only | Queue a record and log the text with the file's `TAG` |
| `DIAG_COUNT(code)`, `DIAG_COUNT_N(code, n)` | any, real-time safe | Add to a counter code |
| `DIAG_RAISE(code)`, `DIAG_RAISE_INSTANCE(code, instance)` | any, real-time safe | Raise a `kind: latched` condition |
| `DIAG_CLEAR(code)` | latched: any; records: task only | Clear a condition when it recovers |

The macros check the code's kind and severity at compile time. The core
reports through `core_os`'s diag sink, which calls `diag::report`/`count`/
`raise`/`clear` with an instance id. `blocksArm()`, `disarmsActive()` and
`setDisarmHandler()` connect the codes' `blocks_arm` / `disarms` flags to
arming and the safety monitor.

**Sending (`diag_reporter.hpp`).** `diag::Reporter` owns the diag task. It
sends the Boot event once, then queued records and Cleared events, answers
queries (GetInfo, GetEntries, Clear on 0x23/0x63), and sends CounterDelta
events every `CONFIG_DIAG_COUNTER_PERIOD_MS` (folding in `UartComm`'s error
counters). `main` constructs it last, so Boot carries the final instance
counts and the safe-mode flag; records from startup wait in the queue.

## Kconfig ("Diagnostics (diag)")

Record and query queue depths, the counter period, the reporter poll period,
the task stack and priority, and `DIAG_CONSOLE_LOG`. With `DIAG_CONSOLE_LOG=n`
all first-party console text compiles out (no format strings, arguments not
evaluated); CI builds that variant with zero warnings.

## Rules

- Only the core (through the sink), the module components and `main` report.
  The hardware layers return errors and don't depend on `diag`.
- A recoverable condition is reported once when it starts and cleared on
  recovery, not on every occurrence. Real-time code only counts or raises.
- Stage and cause args are wire contract: named constants next to the report,
  documented in `diag_codes.yaml`, never renumbered. Retired codes and sources
  (`motor_manager` 0x10, `time_sync` 0x11) are never reused.
- Pure codecs and the slot table (`diag_protocol.cpp`, `diag_table.cpp`) are
  covered by the native tests.
