# Firmware diagnostics

The [code registry](../../../protocol/defs/diag_codes.yaml) defines each diagnostic’s kind, severity and safety flags. Use the [reporting macros](include/diag.hpp) at firmware call sites for compile-time checks. The [core_os diagnostic sink](../core_os/include/core_os/diag_sink.hpp) passes instance identity through to the same diagnostic table.

## Reporting context

| Code kind | Operation | Execution context |
|---|---|---|
| Record | Report an error or warning. Clear a recoverable record when its condition ends. | Task outside the real-time path. Reporting and clearing can enqueue events. |
| Counter | Add one or more occurrences. The reporter publishes counter deltas periodically. | Real-time safe. |
| Latched condition | Raise a condition or clear it on recovery if the code permits clearing. | Both transitions are real-time safe. Atomic dirty bits signal pending changes to the reporter. |

## Reporter and backpressure

The [reporter](src/diag_reporter.cpp) sends startup and diagnostic events, answers host queries, and folds UART error counters into periodic updates. Message formats and query semantics live in the [diagnostic wire contract](../../../docs/protocol/diag.md).

- **Queued records and record clears:** the reporter peeks at the head and removes it only after UART TX accepts it. A full TX queue leaves that event pending for the next pass. The diagnostic queue itself is bounded, so a new record can still be dropped at admission.
- **Latched transitions:** each slot tracks pending raise and clear bits. Repeated transitions coalesce. If both bits are pending, the reporter emits the current state last. If TX fills partway through, it restores only the unsent bits.

[Kconfig](Kconfig) controls queue depths, reporter scheduling, counter publication and console logging. With `DIAG_CONSOLE_LOG=n`, format arguments are type-checked but never evaluated. Perform required side effects before the macro call. The numeric record still reaches the reporting path.
