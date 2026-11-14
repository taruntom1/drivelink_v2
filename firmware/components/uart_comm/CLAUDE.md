# UART transport rules

The [API README](README.md), [firmware rules](../../CLAUDE.md) and
[framing spec](../../../docs/protocol/framing.md) cover callers and wire layout.

- Share encoding/CRCs/scanning with `protocol/`; driver events only wake RX.
  Keep stale-frame timing and logging in Parser, not the clock-free Scanner.
- Never flush RX on a rejected frame: valid following frames may be buffered.
  Reject bad HCRC/length before awaiting payload, then rescan one byte later.
- Keep TX allocation-free after setup: preallocated command buffers and
  latest-wins stream slots. `sendData` uses a single try-lock; busy is
  `ESP_ERR_TIMEOUT`/`stream_skipped`, never a wait. Pool exhaustion is `ESP_ERR_NO_MEM`.
- Time stamping accepts only exact time-v1 replies. Pool entries own stamp
  metadata before publication and clear it before reuse; TX patches timestamp/CRC
  immediately before driver submission. No deferred callbacks/caller pointers,
  ping-specific UART drain or patching ordinary frames. Driver/FIFO backlog
  remains uncertainty; see [time.md](../../../docs/protocol/time.md).
- HIL tests are in `firmware/test/uart_comm/`; see
  [test rules](../../test/CLAUDE.md).
