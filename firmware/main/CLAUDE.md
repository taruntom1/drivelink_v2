# Startup rules

These add to [firmware rules](../CLAUDE.md). Bench, normal, XiP and OTA-candidate
commands are in [the development guide](../../docs/development.md#guarded-bench-builds);
the bench and its ports are in [bench.md](../../docs/bench.md), the board's
current state in [host-status.md](../../plans/host-status.md).

- Preserve startup order: diag, UART, NVS/core, CoreLink, then Reporter, so the
  Boot event has final instance counts/safe-mode state. Report partial failures
  and leave the console usable; destructors tolerate partial setup.
  `app_main` never returns, including when UART fails.
- Core/large startup objects belong on the heap. Instance `start()` runs on
  main's small stack; check its hardware stack mark after startup changes.
- Log loop stacks by configured id and other tasks by the explicit name list.
  Never use `uxTaskGetSystemState()`: scanning stacks in its critical section
  stalls the other core's control loop.
- Bench options stay off in normal firmware. Use `tools/bench_fw.py` for isolated
  profiles; always restore `normal` after bench/XiP checks and verify the device
  refuses bench commands. Don't trust a restored sdkconfig's old timestamp.
- Check OTA boot state before loops start. Pending/unreadable state blocks
  ARM while leaving recovery available; startup never confirms a candidate.
  No separate time or OTA task. See [ota.md](../../docs/protocol/ota.md).
