# UART transport rules

The [API README](README.md), [firmware rules](../../CLAUDE.md) and
[framing spec](../../../docs/protocol/framing.md) cover callers and wire layout.

- Share encoding/CRCs/scanning with `protocol/`; driver events only wake RX.
  Keep stale-frame timing and logging in Parser, not the clock-free Scanner.
- Preserve buffered RX bytes after a frame rejection so the scanner can recover
  valid frames already received.
- Keep TX allocation-free after setup: preallocated command buffers and
  latest-wins stream slots. `sendData` makes one bounded attempt without waiting
  or retrying. A busy slot increments `stream_skipped` and returns `ESP_ERR_TIMEOUT`.
  Command pool exhaustion returns `ESP_ERR_NO_MEM`.
- Each command pool entry owns its timestamp metadata before publication to TX.
  Clear the metadata before reuse. Do not retain caller pointers or defer callbacks
  for timestamping. Follow the [time protocol](../../../docs/protocol/time.md)
  for reply eligibility and timestamp handling.
- HIL tests are in `firmware/test/uart_comm/`; see
  [test rules](../../test/CLAUDE.md).
