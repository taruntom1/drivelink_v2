# Startup rules

These add to [firmware rules](../CLAUDE.md).
The bench and its ports are in [bench.md](../../docs/bench.md), the board's
current state in [host-status.md](../../plans/host-status.md).

- Preserve startup order: diag, UART, NVS/core, CoreLink, then Reporter, so the
  Boot event has final instance counts/safe-mode state. Report partial failures
  and leave the console usable; destructors tolerate partial setup.
  `app_main` never returns, including when UART fails.
  The core starts through `core::bringUp()`: the crash counter goes up before
  anything is sized from saved settings. Nothing large is allocated between
  its heap reading and the running core without a place in the RAM estimate.
- Core and other large startup objects belong on the heap. Instance `start()`
  runs on the main task's small stack. Measure its high-water mark on hardware
  when a change could increase peak stack use or reduce the available stack.
  If hardware is unavailable, record the pending measurement in host status.
- Log loop stacks by configured id and other tasks by the explicit name list.
  Never use `uxTaskGetSystemState()`: scanning stacks in its critical section
  stalls the other core's control loop.
- Keep bench options disabled in normal firmware. Follow the
  [guarded bench procedure](../../docs/development.md#guarded-bench-builds)
  for bench profiles, restoration and verification.
- Check OTA boot state before loops start. Pending/unreadable state blocks
  ARM while leaving recovery available; startup never confirms a candidate.
  No separate time or OTA task. See [ota.md](../../docs/protocol/ota.md).
