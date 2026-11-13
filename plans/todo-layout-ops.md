# To-do (firmware): loop choice at `ADD_INSTANCE`, and removing an instance others are wired to

Status: **closed on 2026-11-13.** Nothing is left to schedule. The text below the outcome is the original proposal, kept as the record.

## Outcome

1. **Loop at add: built.** `ADD_INSTANCE` is `type | loop | label` (F3a-1, F3b). Loop `0xFF` picks the lowest-numbered compatible loop, and a loop the type does not allow answers `LoopMismatch`. `LIST_INSTANCES` reports each instance's loop. Since F3d the loop is a loop instance id, and a type's allowed loops come from its `realtime` requirement. See `docs/protocol/core.md`.
2. **Remove with dependents: the firmware refuses it** (`Referenced`), for an instance that another one is wired to and for a loop that still holds an instance. The user decided on 2026-11-13 that the GUI follows the firmware: Remove is disabled and names what must be rewired first. The GUI rework is phase G7b in [gui-design.md](gui-design.md).

## Why

[firmware-architecture.md](firmware-architecture.md) says "each module type declares which loop classes it may run in, the layout assigns each instance a loop". But the layout family has `ADD_INSTANCE(type, label)` with no loop, and no item that holds an instance's loop. For a type that may run in more than one loop (the mocks can run in `control` or `sensor`) there is no way to choose.

The plan also doesn't say what `REMOVE_INSTANCE` does when another instance has a `Ref` param pointing at the removed one (removing `left_pwm` while `left_drive.actuator` points at it). Layout validation runs when an op is staged, so the op could be refused, or accepted with `layout_invalid` set.

## What the GUI design assumes (G4)

- **Add instance** shows a Loop combo when the type allows more than one loop class, and fixed text when it allows one.
- **Remove with dependents** is allowed. The GUI shows a confirm dialog that lists the dependents. The remove is staged, the dependents' `Ref` params point at an instance that is pending removal, and the device sets `layout_invalid`, so `ARM` is refused until they are rewired or `DISCARD_PENDING` drops the change. If the board reboots like that, the dependents are left out at boot (`core.DependencyMissing`).

## Proposal

1. **Loop at add:** `ADD_INSTANCE(type, label, loop u8)`. The device refuses a loop the type does not allow (`Result` `Invalid`, or a new `Detail` such as `LoopNotAllowed`). `LIST_INSTANCES` reports each instance's loop (it needs it anyway for the tree by loop). Changing an instance's loop later is out of scope: remove and add again.
   - Alternative: a per-instance system param `loop` (`reboot_required`), set after the add. Fewer new ops, but the instance briefly sits in a default loop and every wiring check depends on the order of the two writes.
2. **Remove with dependents:** stage the remove, mark the dependents' wiring invalid and set `layout_invalid`, as above. Don't refuse it: replacing an actuator would otherwise need a dummy instance to rewire to first.

## What it touches (one change)

- `protocol/defs/`: the `ADD_INSTANCE` body and the `LIST_INSTANCES` entry, then the generated codecs and their round-trip tests
- firmware core: the layout family handlers, the layout blob (each instance's loop), and layout validation (loop allowed for the type; dangling `Ref` sets `layout_invalid`)
- `docs/protocol/`: the layout family spec
- `libdrivelink` and `drivelink-cli`: `add` takes a loop
- GUI (later, H2): the Add instance dialog and the Remove dialog
- tests: native core tests for layout validation

## Open questions

- A new `Detail` code for a loop the type doesn't allow, or reuse `LoopMismatch`?
- Should the device default the loop when the type allows only one, so the host can send `0xFF` for "the type's loop"?
