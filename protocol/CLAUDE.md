# Shared protocol rules

These add to the root rules. Definitions/schema details are in
[definitions.md](../docs/protocol/definitions.md); wire contracts live in
`docs/protocol/`.

- Pure C++20: no IDF/RTOS/OS headers, `CONFIG_*`, logging or clocks. No blocking,
  exceptions, RTTI or allocation after construction (Scanner may allocate once).
- Bounds-check bytes before access. Scanner payloads are views valid only until
  its next call. Descriptor enums in `defs.hpp` are wire values; changing this
  hand-written format also changes the generator. Firmware payload capacity
  must meet `kDescriptorMinPayload`.
- Namespace is `drivelink::protocol`; follow existing naming conventions.
  Generated headers stay pure/constexpr/allocation-free.
- Wire changes also update the Python HIL reference
  (`firmware/test/uart_comm/protocol.py`) in the same PR.
- Time pings explicitly drop/count malformed input instead of ordinary error
  replies; see [time.md](../docs/protocol/time.md).
- Keep full-file SHA, appended ESP image digest and ELF identity distinct.
  OTA's `OtaOffset` WRITE rejection alone has a four-byte error body; other
  rejected-command bodies remain empty. See [ota.md](../docs/protocol/ota.md).
- Cover codecs with round-trip, exact-length/malformed-input and scanner resync
  tests in `protocol/test/`; add new test sources to the firmware native build.
  Generator changes need tests in `tools/gen/tests/`.
