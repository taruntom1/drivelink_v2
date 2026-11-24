# drivelink-cli

Generic command-line client over [libdrivelink](../libdrivelink/README.md). `drivelink-cli --help` is the command and argument inventory and does not open a port. This README covers the contracts `--help` does not state.

## Invocation

Global options precede the command: `--endpoint tcp://HOST:PORT|serial://PORT`, `--baud` (default 921600), `--timeout-ms` (default 1500), `--json`, and the agent options in [Through an agent](#through-an-agent).

```sh
drivelink-cli --endpoint tcp://127.0.0.1:8765 --json types
drivelink-cli --endpoint serial://COM3 set 112:0:1=0.2
drivelink-cli --endpoint tcp://127.0.0.1:8765 action 112:0:768 0.5
```

A REF is `TYPE:INSTANCE:ITEM`; IDs accept decimal or `0x` hexadecimal. The numeric IDs in examples are mock-module values; discover real ones with `types`, `items TYPE`, `enums TYPE ITEM` and `codes`. `get`, `get-pending` and `set` accept several REFs in one batch.

Values are parsed with the item's device descriptor: `true`/`false`, integers, floats or enum names. NaN and infinity are rejected. GET and SET print raw slot bits and typed values. `action REF [ARG...] [--blob HEX]` takes descriptor-typed scalar arguments and up to 512 blob bytes.

Every connection reads INFO and checks core v10 before the requested command runs, so a mismatched device fails before any mutation.

## Output and exit codes

Default output is readable key/value text. With `--json`, stdout holds only `{"ok":true,"data":...}` or `{"ok":false,"error":...,"exit_code":...,"result":...,"detail":...}`. Watch commands (`status`, `events`, `sub-stream`) print one envelope per line (NDJSON). `sub-csv` and `capture-read --csv` print CSV to stdout; omit `--json` for them. `ota-upload` reports progress on stderr.

| Exit | Meaning |
|---|---|
| 0 | Success |
| 1 | Internal failure |
| 2 | Usage or value error |
| 3 | Connection |
| 4 | Timeout |
| 5 | Device or agent rejection (`result`, `detail` set) |
| 6 | Malformed or incompatible protocol |

## Writes

No write is retried, except that `ota-upload` resends identical WRITE chunks at the same offset after STATUS confirms the acknowledged offset; it reconciles CONFIRM through STATUS and never repeats REBOOT. A write timeout has an unknown outcome. `cmd-run` and `testgen-run` still attempt DISARM when an ARM reply is lost.

A parameter write that needs a reboot succeeds with `pending_reboot: true`. Layout edits (`add`, `remove`, `label`) also stay pending; `instances pending` shows them. `save` then `reboot` applies additions, removals and reboot-required settings. `discard` drops pending edits; `revert` restores saved live values and the device name, not pending edits. `name [TEXT]` reads or renames the device and works while armed; `name ""` clears it. See [System](../../docs/protocol/core.md#system-family-1) for name limits and persistence.

## Layout and loops

Loops are built-in type 5. `add 5 fusion` returns the allocated loop id; use that id rather than assuming one. Add a module to a loop with `add TYPE LABEL LOOP`, then `save` and `reboot`.

```sh
drivelink-cli --endpoint tcp://127.0.0.1:8765 add 5 fusion        # returns the loop id, e.g. 2
drivelink-cli --endpoint tcp://127.0.0.1:8765 set 5:2:1=250 5:2:2=true
drivelink-cli --endpoint tcp://127.0.0.1:8765 add 112 motor 2
drivelink-cli --endpoint tcp://127.0.0.1:8765 save
drivelink-cli --endpoint tcp://127.0.0.1:8765 reboot
```

`info` reports running loop ids and flags. Status overruns are objects with `loop_id` and `overruns`. Safe mode reports no running loops while `instances` still lists the offline configuration.

## Sources

Source parameters have `is_source: true` in item JSON. Set one with the packed numeric reference described in [Sources](../../docs/protocol/core.md#sources-f4c), then `save` and `reboot`; `0` disconnects it. For example, `set 115:0:2=1912602880` connects fusion.mock instance 0's IMU source to imu.mock instance 0's yaw (type 114, signal 256). Read `<name>.value` and `<name>.age_us` on the consumer like any other item.

## Groups and commands

`groups` lists slots and limits; read capacities with `get 0:0:18 0:0:19 0:0:20`. `sub-config GROUP LOOP DIVIDER REF...` and `cmd-config GROUP LOOP REF...` return the new epoch and channel. `sub-stream GROUP [COUNT]` prints typed samples and drops stale epochs; `sub-csv GROUP COUNT` writes `timestamp_us`, counter and typed values.

`cmd-send GROUP COUNTER [VALUE...]` sends one frame; no values sends a keepalive. Command streams have no acknowledgement, so it drains its frame with an ordered GET_INFO before closing. The device ignores commands while disarmed.

`cmd-run GROUP PERIOD_MS COUNT [VALUE...]` arms, sends repeated frames and disarms when done, on SIGINT or on error. `keepalive GROUP PERIOD_MS COUNT` sends prefix-only frames without arming or taking over an existing arm session. PERIOD_MS must be less than half `watchdog_ms`. Both stop on connection errors; supervise with `status` or diagnostics. ARM failures include the device result and detail, and `ack-fault` never clears an active cause. See [Arming and safety](../../docs/protocol/core.md#arming-and-safety-f4b).

## Capture

`capture-config LOOP DIVIDER SAMPLES PRE_TRIGGER TRIGGER REF...` takes TRIGGER `manual`, `testgen_start`, `value_change REF` or `threshold REF rising|falling VALUE`; VALUE is parsed with the trigger item's descriptor. `capture-wait [TIMEOUT_MS]` returns once the capture is done, with the CaptureDone event when it arrived during the call. `capture-read` pages every sample of a done capture. `capture-read --csv` prints `index,time_us,REF...` with time relative to the trigger sample, `(index - trigger_index) × period_us × divider`. See [Capture](../../docs/protocol/core.md#capture-family-6-f7a).

## Test signals

`testgen-start LOOP REF WAVE OFFSET AMPLITUDE F0 F1 DURATION_MS REPEAT` needs an armed device and an independently fed command watchdog; `testgen-wait` does not feed it. Use zero for unused frequencies. `testgen-stop` is idempotent.

`testgen-run GROUP PERIOD_MS TIMEOUT_MS LOOP REF WAVE OFFSET AMPLITUDE F0 F1 DURATION_MS REPEAT` arms, feeds prefix-only keepalives through an already configured GROUP, starts the test, waits up to TIMEOUT_MS and disarms on completion, interruption or failure. PERIOD_MS must be less than half `watchdog_ms`. See [Test generator](../../docs/protocol/core.md#test-generator-family-7-f7b) for waveforms, target requirements, clamping and hold behavior.

## OTA

`ota-upload IMAGE [--upload-only | --no-confirm] [--window 1..4]` uploads, reboots, verifies the running image and confirms it. `--upload-only` stops before boot selection; `--no-confirm` leaves the candidate unconfirmed. The window defaults to 4. The JSON result is OTA STATUS plus `rebooted` and `confirmed`. Recovery steps are in the [host overview](../README.md) and [OTA](../../docs/protocol/ota.md#safety-recovery-and-host-behavior).

## Time

`time-ping` takes one four-timestamp sample. `time-sync [DURATION_S]` defaults to 180 s (1–3600), sampling every 100 ms for five seconds and every 1 s after; it reports raw samples, estimator snapshots, held-out predictions, timeouts and dropped events. JSON samples add `mapping_valid`, `mapping_generation`, `mapped_host_us` and `mapped_uncertainty_us` from the Session fit; host fields are null when the mapping is invalid. CLI sessions disable background periodic pings. No firmware clock is set.

Through an agent, both commands ping the agent: the `esp_*` fields are agent monotonic times, `mapping_valid` reports the composed device clock, and mapped host fields stay null. For the bare-board stability check, run `host/test/time_smoke.py --cli <cli> --endpoint serial://COM3 --duration 180`, then repeat with `--traffic`. See the [time contract](../../docs/protocol/time.md) for accuracy limits.

## Through an agent

[`drivelink-agent`](../agent/README.md) owns device ports and shares each device with several clients ([agent protocol](../../docs/protocol/agent.md)).

```sh
export DRIVELINK_AGENT_TOKEN=...            # or --token TOKEN
drivelink-cli --endpoint tcp://127.0.0.1:47620 agent-devices
drivelink-cli --endpoint tcp://127.0.0.1:47620 --device base info
drivelink-cli --endpoint tcp://127.0.0.1:47620 --device 02444c000001 --json get 0:0:4
```

`--device ALIAS|ID12` routes every command through the agent. It needs a `tcp://` endpoint and a token from `--token` or `DRIVELINK_AGENT_TOKEN`; otherwise it is a usage error (exit 2). Without `--device` the token is ignored. `--client-name NAME` (default `drivelink-cli`) is shown to other clients, with kind `cli`. Agent refusals (bad token, unknown or offline device, missing lease) exit 5 with the agent's `result` and `detail`.

`agent-devices` (no `--device`) prints `ALIAS online|offline ID NAME ENDPOINT clients=N`, followed by ` holder=CLIENT_ID:NAME (KIND)` when a client holds the lease (`-` for an unknown ID or empty name; KIND is `gui`, `cli`, `ros` or `other`). JSON `data.devices` holds `{alias, online, device_id, name, endpoint, clients, holder_id, holder_name, holder_kind}`; `device_id` is `null` until the agent has read it, and an unheld lease gives `holder_id` 0, empty `holder_name` and `null` `holder_kind`.

`agent-clients` (needs `--device`) prints `CLIENT_ID NAME (KIND)`, with ` [holder]` on the lease holder. JSON `data.clients` holds `{client_id, name, kind, holder}`.

`agent-browse [--timeout-ms N]` takes no endpoint, device, token, lease or arguments (exit 2 otherwise). It runs one IPv4 mDNS browse for `_drivelink._tcp` for N ms (default 1500, 100..10000) and prints `INSTANCE ADDRESS protocol N DriveLink VERSION HOST` per agent (`?` for unadvertised values) or `No agents found.` (exit 0). JSON `data.agents` holds `{instance, host, port, address, endpoint, addresses, from, proto, version}`; `address` is `a.b.c.d:port`, `endpoint` is the `tcp://` form, and `proto` is `null` when absent. Loopback agents and agents started with `--no-advertise` do not appear.

`--lease` (needs `--device`) takes the control lease before the command, preempting and disarming the current holder. Arming, command groups and exclusive operations need the lease or an unheld, disarmed device; otherwise they fail `Busy/LeaseRequired` (exit 5, detail 58). See [The lease](../../docs/protocol/agent.md#the-lease).

Each CLI process is one agent client, so its lease and groups end with the process:

- When the process exits, the agent disarms the device and clears the process's command groups, so `--lease arm` disarms again at once.
- `sub-config` does not outlive its process; `sub-stream` or `cmd-send` in a later process finds the group unconfigured.
- `ota-upload` opens its own session and refuses `--lease`, so OTA through an agent works only while nobody holds the lease.

Use a long-lived client, such as the GUI or an agent-protocol script, to keep a device armed or a group configured.

## Test hooks

`bench-crash` and `bench-stall LOOP DURATION_MS` are refused by normal firmware and normal vdev. Bench-enabled firmware or `drivelink-vdev --allow-crash` accepts them.

## Output compatibility

The CLI consumes typed library results and owns JSON formatting in `src/format.cpp`. CTest `host-cli-golden` compares 60 readable and JSON outputs against vdev; `host-cli-replay` compares 12 deterministic event, action, subscription, capture and CSV outputs. Linux replay also interrupts an in-flight testgen START and requires DISARM plus the original null-status output when no STATUS was read. Only documented dynamic fields are normalized. Run them through the normal host validation; do not re-record fixtures to make a changed implementation pass.
