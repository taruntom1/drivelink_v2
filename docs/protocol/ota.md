# OTA (core v8, family 0x09)

OTA uses ordinary core requests/replies, with the core version/sequence/family/
op header and result/detail. Integers are little-endian. There is no separate
OTA protocol version. Time v1 and diag v3 are unchanged. No image is signed or
encrypted: SHA-256 checks integrity, not authenticity.

## Operations

| Op | Request body | Successful reply body |
|---|---|---|
| BEGIN 1 | size u32, SHA-256 32 bytes | next_offset u32 |
| WRITE 2 | offset u32, length u16, bytes[length] | next_offset u32 |
| FINISH 3 | empty | empty |
| ABORT 4 | empty | empty |
| CONFIRM 5 | empty | empty |
| STATUS 6 | empty | 88 bytes, below |

Exact lengths are required. WRITE length is 1..512; size is 1..slot capacity
(4 MiB on F9). An overrun is rejected, never clamped. Response space is checked
before any side effect. All mutations require disarmed, even ABORT/CONFIRM.
STATUS is read-only and works armed and in safe mode. OTA needs no loop or
working module and can repair firmware while the core is in safe mode.

BEGIN starts the inactive slot with IDF sequential erasure. The SHA is over
every byte of the application .bin, including its appended checksum/hash.
Repeating BEGIN with the identical size/hash in Receiving returns the existing
offset without erasing data; a different active transfer returns Busy/OtaActive.
BEGIN refuses pending confirmation and active capture/testgen. No uploaded
bytes or handles survive a reset; begin again after reconnect.

WRITE at next_offset appends bytes and advances progress only after success.
A duplicate wholly below next_offset is acknowledged only if its bytes match
flash. It neither writes nor hashes them again. A gap, different duplicate,
partial overlap or write beyond the declared size returns
InvalidArgument/OtaOffset (detail 49) **with next_offset u32 as the error body**.
All other rejected commands have empty bodies. Offsets are not sequence IDs;
retried requests use fresh ordinary correlation IDs and identical chunk data.

FINISH needs all declared bytes and the expected complete-file SHA, then IDF
image validation. It prepares the candidate and leaves boot selection untouched.
It does not reboot. Repeating FINISH in Ready succeeds without another write.
Incomplete data returns WrongState; a hash mismatch returns OtaHash and remains
Receiving until ABORT. Backend/IDF image failures enter Error with a diagnostic.

ABORT releases an upload or a prepared Ready image without writing boot
metadata. Idle ABORT is
idempotent. On an unconfirmed running candidate ABORT returns
Busy/OtaPendingConfirm; reset to roll back, or deliberately CONFIRM.
If a commit has already selected another slot, ABORT returns Busy/OtaCommitted
(detail 53); reset rather than attempting to undo the selection.

Use SYSTEM REBOOT after FINISH. After its reply drains, the config caller
selects the prepared candidate and immediately restarts, without another
configuration command. A failed drain leaves Ready unchanged; a failed selection
enters Error, retains the ARM gate and cancels the restart. The reply means
acceptance, not proof of commit or reboot. Query STATUS after an uncertain
outcome; never automatically repeat REBOOT. Other reset paths, including
EXIT_SAFE_MODE, do not commit a prepared image; a physical reset before commit
keeps the confirmed baseline. The new app boots pending confirmation.
The host reconnects, checks GET_INFO/core v8, the running slot, and the full
ELF hash against its uploaded app descriptor, then sends CONFIRM. Firmware
never confirms itself at startup. The next reset without confirmation rolls
back; there is no confirmation timeout. CONFIRM is idempotent on an already
confirmed image. Do not repeat REBOOT merely because its reply was lost: a
second reset of an unconfirmed candidate is itself a rollback request.

## STATUS

The successful body is exactly:

```
state u8 | running_slot u8 | boot_slot u8 | pending_confirm u8
size u32 | next_offset u32 | slot_capacity u32
chunk_bytes u16 | window u8 | backend_error i32 | stage u8
expected_file_sha256[32] | running_elf_sha256[32]
```

Slots are 0/1, pending_confirm is 0/1. Chunk_bytes is min(512, max_payload-10),
accounting for the request header, offset and length. Window is 1..4,
bounded by the config queue depth. State: Idle 0, Receiving 1, Ready 2, Error 3.
Idle has zero size/offset/digest; pending confirmation is an independent boot
flag. Ready keeps the completed transfer's size/hash/progress until reboot or
ABORT. A service reconstructed with a different selected boot slot reports
Ready even when upload metadata is unavailable; if boot_slot differs from
running_slot, ABORT refuses it as already committed. STATUS remains successful in
Error so the host can see the failure; boot fields after an info failure are
the last known values and must not be trusted to confirm an image.

Stages: None 0, Info 1, Begin 2, Write 3, Read 4, Finish 5, Select 6,
Abort 7, Confirm 8. Non-error state has error/stage zero. Error retains the
platform error (ESP-IDF esp_err_t on hardware, -1 natively) and stage.
`core.OtaFailed` (0x0107) reports the same stage in arg0 and offset in arg1.

The running ELF hash is the full 32-byte `esp_app_desc_t.app_elf_sha256`.
GET_INFO build_id remains its first eight bytes, encoded as 16 hex characters.
This hash is distinct from both the SHA of the whole upload and IDF's
`esp_partition_get_sha256` image digest. The host preflights complete unsigned
ESP32-S3 app binaries; IDF performs the authoritative chip/revision validation.

## Safety, recovery and host behavior

ARM returns Busy/OtaActive (47) during Receiving, Ready, Error or uncertain
boot selection, and Busy/OtaPendingConfirm (48) for an unconfirmed running
image. The existing blocks-arm status indication includes the OTA gate.
No timeout clears it. SAVE, REVERT, FACTORY_RESET, layout and PARAM SET mutations
are refused Busy/OtaActive during a transaction, as are new capture ARM and
testgen START. Read-only requests, fast DISARM and explicit REBOOT remain usable.
Backend failure retains the gate until successful ABORT/CONFIRM or reset.

The SDK pipelines at most the advertised window. Every WRITE reply correlates
type/sequence/family/op. Retry only identical chunks, with bounded attempts;
queue Busy/StagingBusy is a no-effect rejection. After a missing reply, STATUS
must describe the same transfer, with progress inside the sent prefix. A
device reset or changed identity aborts recovery. Timed-out IDs are retired
until reconnect; exhausted IDs require reconnect, never unsafe reuse.

Normal CLI upload finishes, reboots, reconnects and confirms only the matching
build. `--upload-only` stops in Ready; `--no-confirm` leaves the booted candidate
pending for a deliberate rollback check. Cancellation before reboot attempts
ABORT; cancellation after reboot leaves the candidate unconfirmed. An uncertain
cleanup reports recovery commands. It never confirms as an error-cleanup step.

## Partition table and simulator

`firmware/partitions.csv` retains NVS at 0x9000/0x6000. Otadata is at
0xF000/0x2000, phy_init at 0x11000/0x1000, ota_0 at 0x20000/4 MiB and ota_1 at
0x420000/4 MiB. Flash is 16 MiB; rollback is enabled; there is no factory slot.
Initial migration requires UART flashing of bootloader/table/app/initial
otadata without erasing NVS. The first baseline must be verified VALID before
testing OTA. Existing A/B configuration formats are unchanged.

Vdev persists simulated slot identities and checked boot metadata in its state
directory. It runs the real core OTA service but keeps executing the same native
program when a simulated candidate boots. GET_INFO keeps the `-vdev` version
suffix so hardware-only preflights still recognize the simulator. It cannot prove ESP image execution,
eFuse compatibility, cache/flash timing, or physical power-loss behavior.
