# F7a implementation plan (approved 2026-11-13)

The capture family, the core event stream and PSRAM, over core wire v6.
Approved by the user before the build. The design and its later decisions live
in `firmware-architecture.md` ("Capture and test signals") and the status file's
decision log. This file records what was approved.

## Scope

One PR. Left for F7b: the test signal generator, the `testgen_start` trigger
(numbered now, refused `Unsupported`), the `testgen_running` status bit and the
`SPIRAM_XIP_FROM_PSRAM` evaluation.

## User decisions before approval

- Add a capture `TRIGGER` op that forces the trigger in any mode.
- Core events: only CaptureDone in F7a, with the shared header
  `version, event, body` and a generated `CoreEvent` enum.
- `input_change` becomes `value_change`, on signals and inputs.

## Wire (core v6)

- Family 6: CONFIG (`loop, divider, samples, pre_trigger, trigger(10), count,
  refs`), ARM, ABORT, STATUS, READ (`first, count`), TRIGGER.
- STATUS: state, flags (configured, irregular), the configuration, recorded,
  trigger index, the loop period at ARM, the trigger timestamp, refs.
- Event 0xC1 CaptureDone; status bits 6..7; GET_INFO capacity;
  `limits.capture_refs` (0x0016); Detail `WrongState` (46).

## Firmware

`CaptureService` in the core, filled at loop step 8; shared `bindSignal` with
the groups; `core_os` capture memory (PSRAM caps alloc, 32 KB internal
fallback); a single-attempt loop claim and Armed publication; the CoreLink
event and status bits; the PSRAM sdkconfig (octal, 80 MHz, caps alloc, ignore
not found).

## Host

libdrivelink capture API and event decoding; CLI `capture-*` with CSV; vdev
forwards the event; host unit test; `host/test/capture_smoke.py` on vdev and
the bare board.

## Checks

Native capture tests, host unit and integration tests, full validation. On the
bare board: capture of mock signals at 1 kHz with PSRAM, the longest tick with
1 and 16 refs, stack marks. No deferred check is new: capture on a real motor
belongs to F7b's step/chirp check.

## Found while building (user decision)

PSRAM makes IDF build the S3 with `-mdisable-hardware-atomics`. PSRAM is kept;
lock-free asserts use `core_os::lockFreeAtomic<T>()`, and no atomic or shared
state may live in PSRAM.
