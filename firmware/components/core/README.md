# Firmware core

Core owns the device layout, module lifecycle, configuration transactions and loop services. The IDF build and native virtual device use the same implementation. [Core wire contract](../../../docs/protocol/core.md) · [Component boundaries](../CLAUDE.md) · [Firmware real-time rules](../../CLAUDE.md)

## Source map

| Change | Entry points |
|---|---|
| Lifecycle and module binding | [Core facade](include/core/core.hpp), [instance context](include/core/instance.hpp), [boot stages](include/core/boot.hpp), [boot](src/core.cpp), [wiring order](src/wiring_order.cpp) |
| Capacity limits and the RAM check | [System settings](../../../protocol/defs/system.yaml), [CoreConfig](include/core/core_config.hpp), [estimate](src/ram_estimate.cpp) |
| Platform configuration | [Kconfig](Kconfig) (the config task), [IDF translation and GPIO policy](src/idf/core_config_idf.cpp) |
| Pending layout and parameters | [Layout validation](src/layout_validation.cpp), [pending copy](src/pending_layout.cpp), [parameter service](src/param_service.cpp) |
| Persistent state | [Blob codec](src/config_blob.cpp), [generation selection and commit](src/persistence.cpp) |
| Command ingress | [CoreLink UART adapter](src/idf/core_link.cpp), [command service](src/command_service.cpp) |
| Loop execution and arming | [Tick](src/loop.cpp), [arm state](include/core/arm_state.hpp), [safety monitor](src/safety.cpp) |
| Loop services | [Groups](src/group_service.cpp), [sources](src/source_service.cpp), [capture](src/capture_service.cpp), [test generation](src/testgen_service.cpp) |
| Firmware updates | [OTA service](src/ota_service.cpp), [platform adapter](../core_os/include/core_os/ota.hpp) |

## Boot and module lifetime

`bringUp()` runs the boot for main and vdev, in three stages:

- `prepareBoot()` decides safe mode and stores the new crash counter, then loads the newest complete generation from NVS, once. Nothing that a saved setting sizes exists yet, so a setting that crashes the boot still reaches safe mode.
- `bootConfig()` applies the saved `reboot_required` system settings to the base `CoreConfig`. In safe mode it applies their defaults.
- Construct Core with that config, then `init()` and `boot(plan)`.

The steps of `boot()` and after:

1. Initialize the registry, then boot with the plan. Use the generation it loaded or the factory layout. A valid saved empty layout stays empty. The system instance has its own storage outside the layout.
2. Validate entries and build records with configuration-owned shadows. Invalid entries can retain records for inspection. Unknown types and duplicate identities have no record and disappear on the next save. An entry beyond capacity keeps an invalid record: the table is sized for the saved layout as well as for the limits. In crash-loop safe mode the default limits run, the layout is judged by the saved ones, and configured records stay offline and editable without creating modules or loop tasks. Exiting safe mode clears the crash counter and leaves reboot to the caller.
3. Start loop tasks with ticks gated. Create modules in stable dependency order, breaking ties by layout index. A failed loop or module leaves dependent instances unavailable while independent loops can start. During instance startup, bind all descriptor parameters, signals and inputs. Binding a parameter initializes its storage from the boot value. Resolve optional C++ interfaces at startup through `queryInterface`.
4. Establish tick order separately from creation order. A reference normally places its target first. A reference marked `RefTicksAfter` can put the actuator after its controller. Resolve sources and allocate cross-loop storage before enabling ticks.
5. Call `start()` to release the gates. Repeated starts while running are idempotent. Construct a new Core after `stop()`.

## Loop execution and safety

Each loop owns its modules' bound scalar storage. The configuration caller owns layout, shadows and command scratch. On device, the single `core_cfg` worker serializes configuration calls. UART RX hands off ordinary commands through a preallocated queue. Time pings use the RX task directly, including in safe mode. [Timestamp contract](../../../docs/protocol/time.md)

Loop IDs can be sparse. Translate the persisted affinity to core 0, core 1 or `kNoAffinity` at creation. Under ESP-IDF, an initially unpinned loop can become pinned when the tick computes floating-point elapsed time. Native real-time ticks wrap staging hooks, actions and module work in the [core_os real-time guard](../core_os/README.md). Device CPU-load reporting samples idle counters once per status packet and handles counter wrap. [CPU sampler](include/core/cpu_load.hpp)

- A tick advances the heartbeat even while paused. Otherwise it observes arm transitions, resets inputs before releasing a new arm generation, applies staged parameters and services the action slot. A pause acknowledgement returns control before any further module access.
- The loop then processes test-generator requests, group mailboxes and incoming source snapshots, emits the generated sample, and ticks modules in wiring order. Same-loop sources refresh immediately before each consumer. Subscription latching, capture and outgoing source snapshots precede publication of the current tick's statistics.
- The configuration caller serializes ARM and fault acknowledgement. RX and diagnostics can disarm concurrently. The arm state, fault latch and disarm epoch use lock-free words. Interpret the two-word ARM timestamp only with a coherent state snapshot.
- The monitor owns heartbeat observations. It reads immutable boot-built loop and actuator lists, atomic state and bounded receipt snapshots. A failed receipt claim leaves the output unchanged. A successful zero timestamp means no frame has arrived. Contention alone cannot establish a timeout.
- Before acting on a timeout or stall, the monitor claims the exact arm generation it observed. Its stop gate serializes ARM commits with emergency force, which repeats while disarmed to cover stale releases. Concurrent emergency stops use hardware force or atomic state. Ordinary transition hooks run on the owning loop. Overflow diagnostic bits preserve active safety conditions when report slots fill.
- Install the diagnostic observer before enabling loops. Stop and fence monitor callbacks and detach the observer before releasing its storage.

## Parameter staging and paused actions

Validate a complete batch before applying it. Each batch belongs to one domain: system/live loop settings, running instances on one loop, offline shadows, or pending configuration. A live rate change restarts the timer on the configuration caller and rolls back if restart fails.

Running-instance writes cross a single staging slot (`Empty → Filling → Ready → Applying → Applied`). After three periods plus 5 ms, the caller can withdraw an unclaimed batch with a definite no-effect outcome. Once the loop claims it, the caller waits for completion, copies applied values into shadows under the mutex, releases the slot, and marks persistent changes dirty. Reads use shadows. The loop never waits for their publication.

A paused action transfers control through a fixed per-loop slot. After pause acknowledgement, a successful action may propose updates to its own parameters. Validate the entire update list, including uniqueness, type, range, armed restrictions and writability, before changing bindings or shadows. Reject structural, read-only and hooked parameters. Set NVS dirty before releasing the pause. Loop actions cannot use this path. [Action buffers](include/core/action.hpp)

## Pending layout and persistence

Create the pending copy lazily from the configured records. Validate the whole layout after each edit using the boot validator. Roll back an edit if its changed entry fails or a previously valid entry becomes invalid. Added instances and reboot-required parameters use pending values. Live values come from configured records. The system instance has no pending layout entry.

The copy also holds the `reboot_required` system settings, starting from the values loaded at boot (in safe mode the saved ones, not the defaults that run). `CoreConfig::configure_system` turns them into limits: each edit validates the layout after it against the new settings and the layout before it against the old ones, so a setting cannot drop below what the pending layout uses, and raising a limit and using it fit in one pending session. These settings have no bound storage. The system shadow holds the running value, and SAVE writes the pending one. DISCARD returns to the boot-loaded values even after a SAVE, like the layout. `CoreConfig::system_type` lets a test add such a setting, and `configure_system = nullptr` fixes a test's limits.

The `limits.*` settings are such settings (`configureLimits()`). Each edit also compares counts with the pending limits, apart from the validator: an entry that is invalid for another reason still counts, so the boot never cuts it off. Then `estimateRam()` must stay within the internal heap that `bringUp()` found free before Core, minus a reserve, unless the edit needs no more than before. The estimate covers the group slots, the loops with their stacks, records, sources and stream slots, a flat allowance per module, and what the platform allocates after Core from Kconfig (CoreLink, the diag reporter, the capture ring without PSRAM). It is never checked at boot: a saved layout boots, and if it cannot, the crash counter leads to safe mode. Measure a new per-group, per-loop or after-Core allocation and add it to [the estimate](src/ram_estimate.cpp).

SAVE writes instance and system blobs to the alternate generation slot, then writes the layout blob as its commit record. Only then clean up removed namespaces. Load requires a valid layout and matching generation for every required blob, including the system. Fall back to the other complete slot before factory defaults. Ignore retired and nonpersistent items, default missing or invalid values, and clamp valid numeric values to current limits. Layout format 3 stores the device name in the commit record. Format 2 loads with an empty name.

REVERT reapplies saved live values through the parameter service in single-domain batches and restores the name after those batches succeed. The config caller owns the writable name. CoreLink injects the immutable device identity from the factory MAC, and native callers supply a stable ID. Factory reset erases configuration namespaces and leaves reboot to the caller.

## Command groups, subscriptions and receipts

Allocate two resolved binding tables per group and configuration scratch before operation, one allocation per group slot (about 4 KB), so a large pool needs no large contiguous block. Normalize group capacity against both configuration and transport payload size. The owning loop swaps configuration and consumes commands before module ticks, then latches subscriptions afterward. Configuration owns LIST shadows.

Tick, RX and transport each make a single atomic ownership attempt per slot. Recheck loop routing after claiming a slot. Rebinding across loops hands off through the old owner. Configuration uses a separate ready/applying/applied handshake with definite withdrawal before claim. A busy slot can skip a telemetry sample. A new sample replaces an unsent one, but a transport may already hold the prior epoch.

Only the owning loop writes input values and their optional receipt timestamps. Value frames carry validated post-ARM RX times. Keepalives leave input receipts untouched. Generated values record their actual write time. Arm transitions reset inputs and receipts together, allowing a controller to require a fresh value after a mode change.

## Cross-loop sources

Bind a source through `bindSource` so the sample and its generated value/age signals share a binding. Missing or failed producers yield invalid samples. Same-loop source edges affect tick order independently of creation order. Each tick entry caches the consumer index for its bindings.

Deduplicate cross-loop refs per ordered producer/consumer pair. Each pair owns three preallocated arrays. Producer and consumer exclusively own their back and front arrays and exchange complete snapshots through an atomic middle index with a dirty bit. Neither retries. The consumer keeps its last snapshot and updates its age from the producer's tick-start timestamp. Freshness means a new snapshot was acquired. Arm transitions preserve source storage.

Only cross-loop values consume pair capacity. The three packed arrays use 12 bytes per distinct value before timestamps and binding metadata. Valid age saturates at `0xFFFFFFFE` microseconds. Invalid samples use `0xFFFFFFFF`.

## Capture

One device-wide capture uses a ring allocated at CONFIG and retained until reconfiguration or destruction. Groups and capture share signal resolution. The owning loop samples only in Armed or Triggered, after subscription latching, and alone advances the capture to Triggered and Done. A failed single-attempt claim during a concurrent ABORT marks the capture irregular.

CONFIG and ARM reject an active capture before contending for its slot. The config caller uses bounded acquisition for CONFIG, ARM and ABORT. Publish Armed with release ordering after preparing the ring and releasing the claim. TRIGGER sets an atomic request. STATUS reads the trigger timestamp only after acquiring Triggered or Done. READ accesses the completed ring in Done. The loop publishes Done before the pending-event flag, and the config caller consumes that event once.

## Test generation

Allocate one slot per loop capacity entry and route by running loop ID, including sparse IDs. The config caller binds the target and publishes START with its arm generation. The loop claims START or STOP before group command consumption and acknowledges after the generation step. A ready request can be withdrawn with no effect. A claimed request completes definitively.

While generation owns a target, groups suppress that input and its receipt but continue applying other entries and feeding the watchdog. After termination, suppress queued commands received at or before the termination boundary even if a mailbox was busy. Switching targets preserves the old target's boundary. Arm transitions clear the gates. A stale generation cannot resume.

Readers take bounded sequence snapshots of atomic words and check the arm generation before reporting an active run. The loop marks its terminal snapshot pending before publishing Done. START waits for the config caller to consume the event by refusing while it is pending. STATUS remains authoritative if delivery fails. The first emitted sample requests capture's same-loop start trigger, consumed on the next due recording sample. Repetitions restart waveform phase without retriggering capture, which continues after generation ends.

## OTA ownership

The config caller owns the transaction and its heap-resident scratch. The platform adapter validates and prepares the inactive image. Only an explicit system reboot commits boot selection, after its reply drains and immediately before restart. Other reset paths skip commit.

Keep the atomic ARM gate independent of loop ticks. It covers an active or failed transaction, uncertainty about the selected boot slot, and a pending running candidate. ABORT cannot undo an already selected image. Consult the [OTA contract](../../../docs/protocol/ota.md) for mutation exclusions, recovery and response bodies.
