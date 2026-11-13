# drivelink-cli

Generic CLI over libdrivelink. `--help` lists commands and arguments.
Options precede the command; supply `--endpoint tcp://127.0.0.1:8765` or
`--endpoint serial://COM3` (921600 baud by default).

```sh
drivelink-cli --endpoint tcp://127.0.0.1:8765 --json types
drivelink-cli --endpoint tcp://127.0.0.1:8765 add 112 motor
drivelink-cli --endpoint tcp://127.0.0.1:8765 set 112:0:1=0.2
drivelink-cli --endpoint tcp://127.0.0.1:8765 save
drivelink-cli --endpoint tcp://127.0.0.1:8765 reboot
drivelink-cli --endpoint tcp://127.0.0.1:8765 action 112:0:768 0.5
```

The numeric mock IDs above are examples. `types`, `items TYPE`, `enums TYPE ITEM`
and `codes` discover definitions. `instances [pending]`, `add`, `remove`, `label`
and `discard` edit the layout. An add is pending until SAVE and reboot.
`get REF...`, `get-pending REF...` and `set REF=VALUE...` support batches, with
Refs written as `type:instance:item`. IDs accept decimal or `0x` hexadecimal.
Param values are parsed from device descriptors (booleans, integers, float or
enum names). GET/SET show raw slot bits and typed values. A pending parameter
write is a successful command with `pending_reboot: true`.

Actions use `action REF [ARG...] [--blob HEX]`, with descriptor-defined scalar
args/results and up to 512 blob bytes. `status [COUNT]` and `events [COUNT]`
watch bounded event queues. `diag-info`, `diag-entries`, `diag-clear`, `save`,
`revert`, `factory-reset`, `reboot` and `exit-safe-mode` use the current firmware
services. `bench-crash` is a test hook rejected by normal firmware and normal
vdev. Every connection checks core v3 before any requested mutation.

Default output is readable key/value text. With `--json`, stdout holds only
`{"ok":true,"data":...}` or `{"ok":false,"error":...,"exit_code":...,"result":...,"detail":...}`.
Watch commands produce one JSON envelope per line (NDJSON). Exit codes: 0
success, 1 internal failure, 2 usage/value error, 3 connection, 4 timeout,
5 device rejection, 6 malformed/incompatible protocol. Help does not open a port.
Writes never retry; a write timeout has an unknown outcome.

Loops are built-in type 5. For example, `add 5 fusion` allocates its loop id;
`set 5:2:1=250` changes its rate, and `set 5:2:2=true` stages its real-time flag.
Use the returned id rather than assuming 2. Add modules with `add TYPE LABEL LOOP`.
SAVE/reboot applies additions, removals and reboot-required settings. `info`
reports running loop ids/flags; status overruns are objects with loop_id and
overruns. Safe mode reports no running loops, while instances lists offline config.

Groups: `groups` lists slots and limits. `sub-config GROUP LOOP DIVIDER REF...`
and `cmd-config GROUP LOOP REF...` return the new epoch/channel. Use
`sub-enable GROUP true|false`, `sub-clear GROUP`, or `cmd-clear GROUP` to change
resources. `sub-stream GROUP [COUNT]` prints typed samples and filters stale
epochs. `cmd-send GROUP COUNTER [VALUE...]` parses configured input types; no
values means keepalive. It drains its frame with an ordered GET_INFO before
closing; the stream itself has no acknowledgement. F4a remains disarmed, so
sending command values cannot change device inputs. NaN/Inf are rejected.
