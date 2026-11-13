# The remaining legacy manager

`time_sync` keeps its protocol-facing task until F8 ports it to the core. F8
deletes this file. The motor managers were removed in F5b and the IMU manager
in F6; git history and F10's planned `legacy-final` tag preserve the legacy firmware.

Do not improve this manager unless it blocks the current step. Firmware-wide
rules in [firmware/CLAUDE.md](../CLAUDE.md) apply. Paths are relative to the repo.

- It registers UART callbacks, owns its task and keeps the payload codec in
  pure `time_sync_protocol`, built with the shared Reader/Writer and native tests.
- MSG_TYPEs are `0x20` (ESP request) and `0x60` (host response), protocol v2.
  ESP-initiated requests carry a `u16 request_id`.
- The wire format is defined by its code and native tests. Deleted READMEs
  remain in history (`git show 7a4d693:components/time_sync/README.md`).
- Destructors unregister callbacks before stopping owned tasks. Blocking
  operations have bounded waits; shutdown wakeups must recheck the stop flag.
- It does not use NVS. F8 introduces time ping on the now-free `0x02`/`0x42`.
