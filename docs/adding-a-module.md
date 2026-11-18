# Adding a module type

A new sensor, actuator or controller needs firmware code and a definition
file. The host needs no code: it reads the type's descriptors from the device
at connect. This checklist links to the rules; follow them, not this summary,
where they differ.

## 1. Definition

- [ ] Add `protocol/defs/<type>.yaml`: a dotted `type` name, an unused
      `type_id` (not 0x00 system, not 0x05 loop), `realtime: required`,
      `forbidden` or `any`, and its items: params, signals, inputs, actions
      and sources, with units, ranges, enums and flags (`reboot_required`,
      `locked_while_armed`, `testgen`, `tune`, GPIO pins, `ref` wiring and its
      order). The format is in [protocol/definitions.md](protocol/definitions.md).
- [ ] For a controller, mark its gains `tune: true` (live numeric params) so the
      GUI's Tune page shows them, and an enum that selects which gains act
      (such as a mode) `tune: true` as well. The Tune page needs no code for
      the type; it groups the gains by naming:
      - Name the gains with a dotted prefix per set (`vel.kp`, `vel.ki`,
        `pos.kp`). Each prefix becomes a group (`vel.* gains`).
      - Start the tune-flagged enum's value names with the prefix they select
        (`velocity` selects `vel`, `position` selects `pos`). The group for the
        selected value comes first and open; the others start collapsed but
        stay editable.
      - A group that no value name selects is always open.
      Without a tune-flagged enum, every group is open. A controller appears
      on the page only while it runs in a loop and has a `testgen` f32 input.
- [ ] For plant identification on the Tune page (H2e-3; a step on an
      open-loop input, a first-order-plus-dead-time fit and suggested starting
      gains), follow these names. They are conventions, not wire data:
      - A gain group is a PID when it has `<prefix>.kp` and `<prefix>.ki`
        (optionally `<prefix>.kd`), in the parallel form
        u = kp·e + ki·∫e dt + kd·de/dt. Suggestions go to `kp` and `ki`
        (SIMC), and `kd` gets 0. Other members such as `d_cutoff` stay as they
        are.
      - A response is a signal of the instance with a unit whose name the
        group's prefix starts (`velocity` for `vel.*`). Every such signal is
        captured; the one that settles with the smallest fit error is shown
        first.
      - The step input is a `testgen` input whose range is finite and includes
        0, so it returns to 0 afterwards. The first one without a unit is the
        default (`duty_sp`).
      - The tune-flagged enum value named `open_loop` is held during the step
        and the previous value is written back afterwards. The operator can
        pick another value or leave the mode unchanged.
- [ ] Add the type's diag codes to `protocol/defs/diag_codes.yaml`. Code 1 is
      its `InitFailed`. Set `blocks_arm` / `disarms` on the faults that need
      them, and use `kind: latched` for recoverable conditions.
- [ ] Run `python tools/gen/gen.py` and commit the generated headers and the
      generated `docs/protocol/types/<type>.md`.

Ids, once published, are permanent. Retire an item or code (`retired: true`);
never renumber or reuse it.

## 2. Firmware

- [ ] Put the module in a `firmware/components/mod_<...>/` component with the
      standard layout ([components/CLAUDE.md](../firmware/components/CLAUDE.md)):
      a factory, an `Instance` that binds the generated ids to its storage in
      `start()`, and `tick()`.
- [ ] Keep its logic pure (only `core`, `core_os`, `protocol/` and std) and
      reach hardware through a small backend seam in `src/idf/`, so the native
      tests can run it against a fake.
- [ ] Follow the real-time rules in `tick()` if the type may run in a
      real-time loop: no allocation, no blocking, no logging, bounded work
      ([firmware/CLAUDE.md](../firmware/CLAUDE.md), "Real-time paths").
- [ ] Allocate and claim hardware (pins, pools, buses) only in `start()`, bind
      first and take hardware last, and release everything in the destructor.
      GPIO params go through the core's pin policy.
- [ ] Export its `core::ModuleType` and add it to `kModuleTypes` in
      `firmware/main/main.cpp`. A pure type may also be registered by the vdev
      (`host/vdev/src/main.cpp`).
- [ ] Add native tests for its pure logic under `firmware/test/native/`, and
      list any new pure source there ([test/CLAUDE.md](../firmware/test/CLAUDE.md)).
- [ ] Give the component a README: purpose, the type's behaviour, Kconfig
      options (usually none: settings are params), rules.

## 3. Checks

- [ ] `python tools/validate.py full` passes.
- [ ] On a bare board, an instance can be added, configured, saved and
      reloaded, and a missing device fails cleanly (`InitFailed`, ARM refused).
- [ ] Checks that need the real hardware get a script with a mock mode and a
      row in the status file's "Deferred hardware checks" while that list exists.
- [ ] Measure the loop's longest tick and the stack marks if the type adds
      real-time work or a task.

## 4. Host

- [ ] Nothing in `libdrivelink`, the CLI or the agent.
- [ ] Optionally, a custom GUI panel under `host/gui/modules/<type>/`, matched
      by type name. The generic screens must still work without it.
- [ ] For ROS, only the robot's mapping config names the instance labels and
      item names.
