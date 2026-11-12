# core

The self-describing core of the firmware (see `plans/firmware-architecture.md`). F3a-1 brought the module registry, the instance bind API, the `control` and `sensor` loops and the param service. F3a-2 adds the layout and its pending copy, validation (pins, wiring, labels), NVS A/B persistence, safe mode and the boot sequence. F3b puts the protocol families on top of this C++ API. Nothing here is on the wire yet.

**The core is pure.** It includes no ESP-IDF or FreeRTOS header, no `sdkconfig.h` and no `CONFIG_` symbol. It uses only `core_os` (the OS adapters, NVS included), `protocol/` and the C++ standard library, so all of it but `src/idf/` also builds natively (`firmware/test/native`). Kconfig reaches it through `CoreConfig`, which `src/idf/core_config_idf.cpp` fills.

## API (namespace `core`, headers in `include/core/`)

| Header | What |
|---|---|
| `core.hpp` | `Core`: the facade. `init(module list)`, `boot(KvStore&)`, `start()` / `stop()`, the layout ops, `save()` / `revert()` / `factoryReset()` / `exitSafeMode()`, the state getters, `params()`, `loop(id)`. `kInitStage*` for `core.InitFailed`. |
| `core_config.hpp` | `CoreConfig` (loop task stack, priority and core; `max_instances`; `gpio_allowed_mask`; `crash_clear_delay_us`) and `coreConfigFromKconfig()` (IDF only) |
| `gpio_policy.hpp` | The ESP32-S3 pin policy (`kEsp32s3Allowed`), ported from motor_manager |
| `module_type.hpp` | `ModuleType {desc, create}` and `InstanceConfig` |
| `registry.hpp` | `Registry` (rejects type id 0, duplicates, a null `create`) and `descIsInTable()` |
| `instance.hpp` | `Instance` (`start`, `tick`), `InstanceContext` (the bind API and `ref()`), `InstanceState`, `TickContext`, `OnChange`, `kStartStage*` |
| `instance_table.hpp` | The records, one per configured instance plus the system's |
| `value.hpp` | `checkValue()`, `storeValue()` / `loadValue()`, `valueTypeOf<T>()` |
| `loop.hpp` | `Loop`: the instance list, `runTick()`, the heartbeat, the stats and the staging slot; `loopClassOf()` |
| `param_service.hpp` | `ParamService`: `set()` (`stage()` + `complete()`), `get()`, `Ref`, `SetEntry`, `ParamDomain`, `nvsDirty()` |
| `pending_layout.hpp` | `PendingLayout`: the pending copy and its ops |
| `layout_validation.hpp` | `validateLayout()`, `LayoutEntry`, `refLinks()` (pure) |
| `wiring_order.hpp` | `creationOrder()`, `tickOrder()` (pure) |
| `config_blob.hpp` | The NVS blob codec and `crc32()` |
| `persistence.hpp` | `persist::loadConfig()`, `saveConfig()`, `eraseAll()`, `applyPairs()`, the NVS names |
| `safe_mode.hpp` | `decideSafeMode()` (pure) |
| `status.hpp` | `Result`, `Detail` and `Status {result, detail, index}` with `ok()` and `accepted()` |

## Boot

`main` constructs the `Core`, calls `init(types)`, then `boot(store)`, then `start()`. `boot`:
1. **Safe mode.** The crash counter (RTC no-init memory, `core_os/boot_info.hpp`) goes +1 on a crash reset (panic, interrupt or task watchdog) and back to 0 on any other. At 3 the device boots in **safe mode**: every configured record is `Offline`, nothing is created, the loops run empty, and `core.SafeModeEntered` is reported. Params can still be read and edited, and SAVE keeps the layout. Outside safe mode the counter is cleared `crash_clear_delay_us` (30 s) after the boot; in safe mode it is not, so the device stays safe while the crash loop goes on. `exitSafeMode()` clears it; the reboot is the caller's job, and any non-crash reset ends safe mode too.
2. **Load** the newest complete generation from NVS (below), or the defaults. The system params apply at once.
3. **Validate** the layout (below). An entry that fails is left out (`Invalid`, `core.LayoutInvalid` with the detail, `layoutInvalid()`).
4. **Records.** Every entry gets a record, running or not, holding its shadow (the loaded values). An unknown type, a duplicate (type, inst) or an entry beyond `max_instances` gets none, so the next SAVE drops it.
5. **Create** in dependency order (a stable Kahn sort: dependencies first, then by layout index). A failed `create` / `start` is `Failed` (`core.InstanceStartFailed`); an instance wired to one that is not running is `DependencyMissing` (`core.DependencyMissing`, arg1 the missing one), transitively.
6. **Tick order.** Each loop gets its running instances in the wiring's tick order: a ref's target ticks before the instance, unless the ref is flagged `RefTicksAfter` (YAML `ref_order: after`). A controller with `feedback` and `actuator` (`after`) gives feedback → controller → actuator, whatever the layout order.

Getters for F3b's `GET_INFO`: `safeMode()`, `layoutInvalid()`, `configLoadedFromNvs()`, `pendingReboot()`, `nvsDirty()`, `instanceCount()` (running), `configuredCount()`, `instancesFailed()`, `findRecord()`.

**Instances** bind their storage in `start(ctx)`:
- `ctx.bindParam(id, T*, OnChange = nullptr)`, `ctx.bindSignal(id, const T*)`, `ctx.bindInput(id, T*)`, where `T` is `bool`, `uint8_t`, `uint16_t`, `uint32_t`, `int32_t`, `float` or an enum class with a `uint8_t` base. Each bind is checked against the descriptor; `bindParam` writes the boot value into the storage.
- `ctx.param(id)` reads a boot value (structural params), `ctx.ref(id)` returns the instance a wiring param points at (nullptr when unwired), `ctx.fail(stage, err)` says why `start` failed.
- After `start`, every param, signal and input must be bound, or the instance failed (`core.InstanceStartFailed`, arg1 the stage).

**The system instance** is the core's own storage for the system params (`protocol/defs/system.yaml`, type `0x00`, instance 0). It runs in no loop and is not a layout entry.

## Layout and the pending copy

A layout entry is `{type, inst, loop, label}` plus the instance's params. Changes go to the **pending layout** (`PendingLayout`), which SAVE writes and a reboot applies:
- `addInstance(type, loop, label, &inst)`: the device picks the lowest free instance id of the type (0..254). Params start at their defaults.
- `removeInstance(type, inst)`: refused while another entry is wired to it (`Referenced`).
- `setLabel`, `discardPending`, `getPending(refs, values)` (added instances and `reboot_required` params from the pending values, other params from the records).
- A `reboot_required` param SET, and any SET on an instance that exists only in the pending layout, goes to the pending values and answers `PendingReboot` with the applied values (`Status::accepted()`).
- The pending copy is made at the first op from the configured records. The whole layout is validated on every op; an op is undone and refused when an entry that passed before fails after it, or the entry it added or changed fails.
- Layout ops are refused while armed (`Armed`) and before `boot()`.
- `pendingReboot()`: the pending copy is active, or the last SAVE wrote it.

**Validation** (`validateLayout`, pure; the same code at boot and on every op): the type is registered (`UnknownType`); room (`Capacity`); (type, inst) unique (`Exists`); the loop is one the type allows (`LoopMismatch`); the label non-empty, at most 16 characters and unique (`BadLabel`, `LabelInUse`); every pin (`gpio` param, `0xFF` = none) allowed by `CoreConfig::gpio_allowed_mask` (`GpioNotAllowed`) and not used twice on the device (`GpioConflict`); every wiring value 0 (unwired) or `type << 8 | inst` naming an entry whose type is in the ref's category, the part of the type name before the dot (`BadRef`), in the same loop (`LoopMismatch`); no cycle in the ref graph or the tick graph (`WiringCycle`); and every entry it is wired to valid (`DependencyMissing`).

## Params

`ParamService::set` takes atomic batches of at most 64 entries (`kMaxBatchEntries`):
- Every entry is validated first: the Ref exists, it is a param (`WrongKind`), not `read_only`, not `locked_while_armed` while armed, and the value passes `checkValue` (clamped). A Ref may appear once.
- Every entry must have the same **domain** (else `CrossLoopBatch`): **system** (applied on the caller; a rate change restarts the loop's timer, rolled back on failure with `core.TimerRestartFailed`), **control** or **sensor** (live params of running instances: staged into the loop's slot, which the config task waits up to 3 periods plus 5 ms for, then withdraws), **offline** (an instance that isn't running: written to its shadow on the caller) or **pending** (above).
- `get()` and the applied values come from the shadow the config task keeps, never from loop-owned storage. `stage()` and `complete(timeout)` are public, so tests drive a batch without threads.
- `nvsDirty()` is set by an applied `persist` param or a pending op, and cleared by SAVE.

## NVS A/B persistence

- **Blobs** (`config_blob.hpp`, little-endian through `wire::Reader` / `Writer`, closed by a CRC-32):
  - instance: `fmt u8 | gen u32 | count u16 | count × (id u16, value u32) | crc32`, in namespace `t<type>i<inst>` (`t00i00` is the system), keys `p_a` / `p_b`
  - layout: `fmt u8 | gen u32 | count u8 | count × (type u8, inst u8, loop u8, label[16]) | crc32`, in namespace `core`, keys `layout_a` / `layout_b`
- **SAVE** (`save()`): refused while armed. Generation `newest + 1` goes to slot `gen & 1`: every instance blob (every `persist` param of every record, or of the pending layout when there is one), the system's, then the layout blob as the commit record. Then the namespaces of instances no longer in the layout are erased. A failed write reports `core.NvsWriteFailed` and answers `Fault` / `StorageFailed`; the previous generation stays intact, so a power loss at any point leaves it loadable.
- **Load**: the valid layout slot with the highest generation, with every instance blob (and the system's) of that same generation; else the other slot's generation; else the defaults with `core.ConfigLoadFailed` (not on a blank store). Unknown and retired ids and non-`persist` params are ignored, a missing id takes its default, an invalid value its default, and a value out of range is clamped.
- **REVERT** (`revert()`): reloads every live, persisted param of the configured instances and the system from the newest complete generation, through `ParamService::set`, per domain in batches of at most 64. `NoSavedConfig` when there is none.
- **FACTORY_RESET** (`factoryReset()`): erases `core` and every `t??i??` namespace. Refused while armed; the reboot is the caller's job.

## Kconfig

| Option | Default | Range |
|---|---|---|
| `CORE_MAX_INSTANCES` | 16 | 1–64 |
| `CORE_CONTROL_TASK_STACK` / `_PRIO` / `_CORE` | 4096 / 20 / 1 | 2048–16384 / 1–24 / -1–1 |
| `CORE_SENSOR_TASK_STACK` / `_PRIO` / `_CORE` | 4096 / 10 / 0 | 2048–16384 / 1–24 / -1–1 |

`CoreConfig::gpio_allowed_mask` is the ESP32-S3 policy minus the uart_comm TX/RX pins (`CONFIG_UART_COMM_TX_PIN` / `_RX_PIN`). `crash_clear_delay_us` is 30 s.

## Rules

- **Pure.** No IDF, FreeRTOS, `sdkconfig.h` or `CONFIG_` outside `src/idf/`. Anything OS-specific goes through `core_os`.
- **Real-time paths:** the loop tick, the staging swap and the `OnChange` hooks take no lock, don't log, allocate or block, and do bounded work.
- **Allocate only at setup or configure time:** the `Core` constructor, `boot()`, the layout ops, SAVE, REVERT and FACTORY_RESET. A live param write allocates nothing.
- **Flash stalls both cores.** SAVE and FACTORY_RESET write NVS, which is why they are refused while armed.
- **No logging.** The core reports through the `core_os` diag sink; `main` logs the startup summary.
- Diag args are wire contract: the stages and causes (`kInitStage*`, `kStartStage*`, `persist::kNvsOp*`, `persist::kLoad*`) are documented in `protocol/defs/diag_codes.yaml` and never renumbered. `core.LayoutInvalid` carries a `Detail`, which F3b numbers.
