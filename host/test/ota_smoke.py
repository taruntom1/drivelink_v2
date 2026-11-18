"""F9 shared OTA campaign: vdev or bare board, no motor/IMU needed.

Hardware requires the reviewed F9 partition table/baseline already installed
over UART, plus its application .bin and a distinguishable IDF-built candidate.
Synthetic images in simulated() are for vdev only and must NEVER be flashed.
The final step restores the baseline via OTA and confirms it. On failure the
script reports the current state and leaves explicit recovery commands.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time


class Cli:
    def __init__(self, path, endpoint):
        self.path, self.endpoint = str(path), endpoint

    def __call__(self, *args, code=0, timeout=360):
        result = subprocess.run([self.path, '--endpoint', self.endpoint, '--json', *map(str, args)],
                                capture_output=True, text=True, timeout=timeout)
        data = json.loads(result.stdout)
        assert result.returncode == code and data['ok'] == (code == 0), (args, result.returncode, data, result.stderr[-1000:])
        return data.get('data', data)

    def reboot(self):
        # Readiness polling never reissues REBOOT: a second reboot of a pending
        # image would itself cause rollback.
        self('reboot')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                return self('ota-status')
            except (AssertionError, json.JSONDecodeError):
                time.sleep(.1)
        raise AssertionError('device did not return after reboot')


def identity(image):
    data = Path(image).read_bytes()
    assert len(data) >= 288 and data[0] == 0xe9 and struct.unpack_from('<H', data, 12)[0] == 9
    assert struct.unpack_from('<I', data, 32)[0] == 0xabcd5432
    return data, data[176:208].hex()


def run(cli_path, endpoint, baseline, candidate, stack_wait=0):
    cli = Cli(cli_path, endpoint)
    original = cli('instances')
    assert all(x['type'] == 5 for x in original), 'requires a loops-only bare-board layout'
    info = cli('info')
    assert info['protocol_version'] == 10 and not (info['flags'] & 9), info
    refs = [f"5:{loop['id']}:1" for loop in info['loops']] + ['0:0:3', '0:0:4']
    settings = cli('get', *refs)['values']
    a, hash_a = identity(baseline)
    b, hash_b = identity(candidate)
    assert hash_a != hash_b, 'baseline and candidate must have different ELF build hashes'
    start = cli('ota-status')
    assert start['state'] == 0 and not start['pending_confirm'] and start['elf_sha256'] == hash_a, start
    assert cli('arm', code=5)['detail'] == 41, 'requires no command group or active fault'
    try:
        # Explicit retry and rejection contracts on the actual production ops.
        digest = hashlib.sha256(b).hexdigest()
        assert cli('ota-begin', len(b), digest)['offset'] == 0
        assert cli('arm', code=5)['detail'] == 47
        assert cli('ota-write', 0, b[:512].hex())['offset'] == 512
        assert cli('ota-write', 0, b[:512].hex())['offset'] == 512
        changed = bytes((b[0] ^ 1,)) + b[1:512]
        assert cli('ota-write', 0, changed.hex(), code=5)['detail'] == 49
        assert cli('ota-write', 600, b[:32].hex(), code=5)['detail'] == 49
        assert cli('ota-finish', code=5)['detail'] == 46
        cli('ota-abort')
        # A reset during a partial upload must keep the confirmed app.
        cli('ota-begin', len(b), digest); cli('ota-write', 0, b[:512].hex())
        partial = cli.reboot()
        assert partial['elf_sha256'] == hash_a and not partial['pending_confirm'] and partial['state'] == 0, partial
        cli('ota-begin', 512, '00' * 32); cli('ota-write', 0, b[:512].hex())
        assert cli('ota-finish', code=5)['detail'] == 50
        cli('ota-abort')
        assert cli('ota-status')['boot_slot'] == start['running_slot']

        ready = cli('ota-upload', candidate, '--upload-only')
        assert ready['state'] == 2 and ready['boot_slot'] == start['running_slot'], ready
        if stack_wait:
            # Let the normal console logger observe the config task's deepest
            # OTA stack before a reboot discards its high-water mark.
            time.sleep(stack_wait)
        assert cli('arm', code=5)['detail'] == 47
        cli('ota-abort')
        aborted = cli.reboot()
        assert aborted['elf_sha256'] == hash_a and not aborted['pending_confirm'], aborted

        confirmed = cli('ota-upload', candidate)
        assert confirmed['confirmed'] and confirmed['elf_sha256'] == hash_b, confirmed
        if info['firmware_version'].endswith('-vdev'):
            assert cli('info')['firmware_version'].endswith('-vdev'), 'OTA must preserve simulator identity for hardware preflights'
        assert cli.reboot()['elf_sha256'] == hash_b
        pending = cli('ota-upload', baseline, '--no-confirm', '--window', 1)
        assert pending['pending_confirm'] and pending['elf_sha256'] == hash_a, pending
        assert cli('arm', code=5)['detail'] == 48
        assert cli('ota-abort', code=5)['detail'] == 48
        rollback = cli.reboot()
        assert rollback['elf_sha256'] == hash_b and not rollback['pending_confirm'], rollback
        restored = cli('ota-upload', baseline)
        assert restored['confirmed'] and restored['elf_sha256'] == hash_a, restored
        assert cli('instances') == original
        assert cli('get', *refs)['values'] == settings
        assert cli('arm', code=5)['detail'] == 41
        print('PASS OTA duplicate/conflict/gap/hash checks, partial reset, FINISH/ABORT, confirmed boot, rollback, NVS and baseline restoration', flush=True)
    except Exception:
        try:
            state = cli('ota-status')
            print(f'OTA failed; current state: {state}', flush=True)
            if not state['pending_confirm']:
                cli('ota-abort')
        except Exception as error:
            print(f'OTA cleanup uncertain: {error}', flush=True)
        print('Recovery: inspect ota-status; ota-abort before reboot, or reboot an unconfirmed candidate to roll back. Restore normal with tools/bench_fw.py normal.', flush=True)
        raise


def simulated(cli_path, endpoint):
    """Same campaign with structurally valid non-executable native fixtures."""
    assert Cli(cli_path, endpoint)('info')['firmware_version'] == '0.1.0-vdev', 'synthetic images require a fresh vdev baseline'
    def fixture(seed):
        data = bytearray(4176)
        data[0] = 0xe9; data[1] = 1; data[12] = 9; data[23] = 1
        struct.pack_into('<II', data, 24, 0x3c000020, 4096)
        struct.pack_into('<I', data, 32, 0xabcd5432)
        data[48:64] = b'native-candidate'
        data[176:208] = bytes((seed + i) & 255 for i in range(32))
        checksum = 0xef
        for value in data[32:4128]:
            checksum ^= value
        data[4143] = checksum
        data[4144:] = hashlib.sha256(data[:4144]).digest()
        return data
    with tempfile.TemporaryDirectory(prefix='ota-fixtures-') as folder:
        # Exercise the real CLI file-open boundary outside Windows ANSI paths.
        unicode_dir = Path(folder) / '\u00e9-\U0001f697'
        unicode_dir.mkdir()
        baseline, candidate = Path(folder) / 'native-a.bin', unicode_dir / 'firmw\u00e9.bin'
        baseline.write_bytes(fixture(11)); candidate.write_bytes(fixture(77))
        Cli(cli_path, endpoint)('ota-upload', baseline)
        run(cli_path, endpoint, baseline, candidate)
        if endpoint.startswith('tcp://'):
            from ota_cli_failure import check, pipeline_reboot
            cli = Cli(cli_path, endpoint)
            check(cli_path, endpoint, candidate, boot_reconnect=True)
            cli('ota-upload', baseline)
            check(cli_path, endpoint, candidate, hold_commit=True)
            ready = cli('ota-status')
            assert ready['state'] == 2 and ready['boot_slot'] == ready['running_slot'], ready
            cli('ota-abort')
            pipeline_reboot(cli, endpoint, baseline, candidate)
            check(cli_path, endpoint, candidate, mismatch=True)
            assert cli('ota-status')['pending_confirm']
            rolled_back = cli.reboot()
            assert rolled_back['elf_sha256'] == identity(baseline)[1] and not rolled_back['pending_confirm'], rolled_back


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint', required=True)
    ap.add_argument('--baseline', required=True)
    ap.add_argument('--candidate', required=True)
    ap.add_argument('--stack-wait', type=float, default=0, help='hold Ready for console stack observation before reboot')
    args = ap.parse_args()
    run(args.cli, args.endpoint, args.baseline, args.candidate, args.stack_wait)
