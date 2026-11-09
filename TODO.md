# TODO

## Convention fixes

These are fixes for code that breaks the conventions in `CLAUDE.md` / `components/CLAUDE.md`. Each one is tracked in `review/10-docs-and-conventions.md`, and its status there gets updated in the same change.

**CI**
- Run the host tests (`test/host/`) in CI together with `idf.py build` for `/` and `test/uart_comm` (review G7).

**Host input validation** (review E1, E2)
- Check host GPIOs against an allowed set: no flash/PSRAM/UART0/USB pins, and nothing ≥ `GPIO_NUM_MAX`.
- Reject non-finite gains, limits and setpoints with an error result code.
- Reject setpoints for unknown motor ids in `queueSetpoint()`, on the RX task, so the control tick never sees them.

