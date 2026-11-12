# core

The self-describing core of the firmware (see `plans/firmware-architecture.md`). F3a-1 brings the module registry, the instance bind API, the `control` and `sensor` loops and the param service. F3a-2 adds the layout, validation, NVS persistence, safe mode and the boot sequence; F3b the protocol families. Nothing here is on the wire yet.

**The core is pure.** It includes no ESP-IDF or FreeRTOS header, no `sdkconfig.h` and no `CONFIG_` symbol. It uses only `core_os` (the OS adapters), `protocol/` and the C++ standard library, so all of it but `src/idf/` also builds natively (`firmware/test/native`). Kconfig reaches it through `CoreConfig`, which `src/idf/core_config_idf.cpp` fills.

## API (namespace `core`, headers in `include/core/`)

| Header | What |
|---|---|
| `core.hpp` | `Core`: the facade. `init(module list)`, `createInstance(type, inst, loop, label)` (boot only), `start()` / `stop()` (the loop tasks), `params()`, `loop(id)`. `kInitStage*` for `core.InitFailed`. |
| `core_config.hpp` | `CoreConfig` (loop task stack, priority and core; `max_instances`) and `coreConfigFromKconfig()` (IDF only) |
| `module_type.hpp` | `ModuleType {desc, create}` and `InstanceConfig` |
| `registry.hpp` | `Registry` (rejects type id 0, duplicates, a null `create`) and `descIsInTable()` |
| `instance.hpp` | `Instance` (`start`, `tick`), `InstanceContext` (the bind API), `TickContext`, `OnChange`, `kStartStage*` |
| `value.hpp` | `checkValue()`, `storeValue()` / `loadValue()`, `valueTypeOf<T>()` |
| `loop.hpp` | `Loop`: the instance list, `runTick()`, the heartbeat, the stats and the staging slot |
| `param_service.hpp` | `ParamService`: `set()` (`stage()` + `complete()`), `get()`, `Ref`, `SetEntry` |
| `status.hpp` | `Result`, `Detail` and `Status {result, detail, index}` |

**Module types** are listed explicitly by `main` and passed to `Core::init()`: there is no static self-registration, which the linker would drop. Each `desc` must be the generated `defs::kTypeTable` entry for its id; main checks that with `descIsInTable()`. The list is empty in F3a-1.

**Instances** are created at boot only. `createInstance` checks the type is registered, the loop is one the type allows (`LoopMismatch`), `(type, inst)` is new, there is room (`CORE_MAX_INSTANCES`) and the label fits 16 characters (uniqueness comes in F3a-2). It calls `create`, then `start(ctx)`. In `start` the instance binds its storage:
- `ctx.bindParam(id, T*, OnChange = nullptr)`, `ctx.bindSignal(id, const T*)`, `ctx.bindInput(id, T*)`, where `T` is `bool`, `uint8_t`, `uint16_t`, `uint32_t`, `int32_t`, `float` or an enum class with a `uint8_t` base.
- Each bind is checked against the descriptor: the id exists, the kind and value type match, and it is not bound twice. `bindParam` writes the boot value into the storage.
- `ctx.param(id)` reads a param's boot value (for structural params); `ctx.fail(stage, err)` says why `start` failed.
- After `start`, every live param, signal and input must be bound. Otherwise the instance failed: `core.InstanceStartFailed` (arg0 `type << 8 | inst`, arg1 the stage) is reported through the `core_os` diag sink and the instance is left out.

**The system instance** is the core's own storage for the system params (`protocol/defs/system.yaml`, type `0x00`, instance 0), bound like an instance. It runs in no loop.

**Loops.** Loop 0 is `control` (core 1, priority 20, 1 kHz by default) and loop 1 is `sensor` (core 0, priority 10, 100 Hz). The rates are the system params `control.rate_hz` (100–2000) and `sensor.rate_hz` (10–500); the period is `1e6 / rate`, rounded. Each loop runs on a `core_os::PeriodicTask` (`core_ctrl`, `core_sens`) and its tick, `Loop::runTick(elapsed)`, does steps 1, 3, 7 and 9 of the plan's tick order:
1. increment the heartbeat (an atomic `u32`)
3. swap in the staged batch, if one is Ready: copy every value, run the `OnChange` hooks, mark it Applied and signal the config task
7. `tick()` each instance in insertion order, with `dt = elapsed × period`
9. update the stats: ticks, overruns (`elapsed - 1`), last and max run time

**Param writes** (`ParamService::set`) are atomic batches of at most 64 entries (`kMaxBatchEntries`):
- Every entry is validated first: the Ref exists, it is a param (`WrongKind`), not `read_only`, not `reboot_required` (refused with `PendingReboot` / `Unsupported` until F3a-2), not `locked_while_armed` while armed, and the value passes `checkValue`. A Ref may appear once.
- `checkValue` rejects a bool other than 0/1, a u8 or u16 that doesn't fit, an unlisted enum value and a non-finite f32; it clamps the rest to min/max. The response carries the applied values.
- A batch is all system params, or all params of instances in one loop (`CrossLoopBatch`).
- A loop batch claims that loop's one staging slot (Empty → Filling → Ready), or gets `Busy` / `StagingBusy`. The config task waits up to 3 periods plus 5 ms for the loop to apply it. On a timeout it withdraws it (Ready → Empty, `Busy` / `Withdrawn`); if the loop took it meanwhile, it waits for Applied. Either way the outcome is definite.
- A system batch is applied on the caller. A rate change restarts the loop's timer; on a failure the rates already set are rolled back, `core.TimerRestartFailed` is reported and the answer is `Fault`.
- `get()` and the applied values come from a shadow copy of every param that the config task keeps, so it never reads storage the loops own.
- `stage()` and `complete(timeout)` are public, so tests drive a batch deterministically without threads.

`Result` and `Detail` are C++ enums for now; F3b gives them wire numbers in `registry.yaml` and keeps the names.

## Kconfig

| Option | Default | Range |
|---|---|---|
| `CORE_MAX_INSTANCES` | 16 | 1–64 |
| `CORE_CONTROL_TASK_STACK` / `_PRIO` / `_CORE` | 4096 / 20 / 1 | 2048–16384 / 1–24 / -1–1 |
| `CORE_SENSOR_TASK_STACK` / `_PRIO` / `_CORE` | 4096 / 10 / 0 | 2048–16384 / 1–24 / -1–1 |

## Rules

- **Pure.** No IDF, FreeRTOS, `sdkconfig.h` or `CONFIG_` outside `src/idf/`. Anything OS-specific goes through `core_os`.
- **Real-time paths:** the loop tick, the staging swap and the `OnChange` hooks take no lock, don't log, allocate or block, and do bounded work. A hook must be bounded and must not allocate.
- **Allocate only at setup:** the `Core` constructor, `createInstance` and `start()`. A param write allocates nothing.
- **No logging.** The core reports through the `core_os` diag sink; `main` logs the startup summary.
- Instances report their own failures through `ctx.fail(stage, err)`, with stages below `0x100`; the core's own stages (`kStartStage*`) are documented in `protocol/defs/diag_codes.yaml`.
