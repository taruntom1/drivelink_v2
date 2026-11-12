"""CLI smoke against main firmware on a bare ESP32. Temporary mocks only.

Refuses pending edits and restores the original configured layout and system
params. Never factory-resets or drives legacy motor hardware.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--report')
    args = ap.parse_args()

    def cli(*command, code=0):
        p = subprocess.run([args.cli, '--endpoint', 'serial://' + args.port, '--json', *command], capture_output=True, text=True, timeout=25)
        assert p.returncode == code, (command, p.returncode, p.stdout, p.stderr)
        result = json.loads(p.stdout)
        assert result['ok'] == (code == 0), result
        return result.get('data', result)

    def connected():
        deadline = time.monotonic() + 15
        while True:
            try:
                return cli('info')
            except AssertionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.3)

    original_info = connected()
    assert not original_info['flags'] & 11, 'pending edits, invalid layout or safe mode; smoke did not modify configuration'
    assert not cli('status')['flags'] & 8, 'unsaved parameter edits; smoke did not modify configuration'
    original_layout = cli('instances')
    rate_refs = [f"5:{loop['id']}:1" for loop in original_info['loops']] + ['0:0:3']
    original_rates = cli('get', *rate_refs)['typed_values']
    assert len(cli('types')) >= 4
    for t in (0, 112, 113, 114):
        assert cli('items', str(t))
    assert cli('codes')
    created = []
    extra_loop = None
    try:
        # Unique labels allow a board with an existing mock layout.
        prefix = 'c' + format(int(time.time()) & 0xFFFFFF, 'x')
        for type_, label in ((112, 'm'), (113, 'e'), (114, 'i')):
            created.append((type_, cli('add', str(type_), prefix + label)['instance']))
        extra_loop = cli('add', '5', prefix + 'l')['instance']
        assert cli('set', f'5:{extra_loop}:1=250')['pending_reboot']
        created.append((114, cli('add', '114', prefix + 'j', str(extra_loop))['instance']))
        motor, encoder, imu = [inst for _, inst in created[:3]]
        wiring = (112 << 8) | motor
        assert cli('set', f'113:{encoder}:1={wiring}')['pending_reboot']
        assert len(cli('instances', 'pending')) == len(original_layout) + 5
        cli('save'); cli('reboot'); connected()
        assert len(cli('instances')) == len(original_layout) + 5
        target_rate = 500 if original_rates[0] != 500 else 1000
        assert cli('set', f'{rate_refs[0]}={target_rate}')['typed_values'] == [target_rate]
        cli('save'); cli('reboot'); connected()
        assert cli('get', rate_refs[0])['typed_values'] == [target_rate]
        cli('action', f'112:{motor}:768', '0.5'); time.sleep(.15)
        assert cli('action', f'112:{motor}:768', '0.5')['typed_values'][0] > 0
        assert cli('action', f'113:{encoder}:768')['typed_values'][0] > 0
        assert cli('action', f'112:{motor}:769', '--blob', '6563686f')['blob_hex'] == '6563686f'
        cli('action', f'114:{imu}:768'); cli('action', f'114:{imu}:769')
        info = cli('info')
        assert next(l for l in info['loops'] if l['id'] == extra_loop)['rate_hz'] == 250
        status = cli('status')
        assert status['msg_type'] == 128
        assert {e['loop_id'] for e in status['overruns']} == {l['id'] for l in info['loops']}
        diagnostics = cli('diag-info'); cli('diag-entries')
        cli('bench-crash', code=5)
        print('PASS CLI serial discovery, descriptors, layout, typed params, SAVE/reboot/reload, actions, status and diag')
    finally:
        # Remove dependents before their providers, then restore system rates.
        for index in (1, 0, 2, 3):
            if index < len(created):
                type_, inst = created[index]
                cli('remove', str(type_), str(inst))
        if extra_loop is not None:
            cli('remove', '5', str(extra_loop))
        cli('set', *(f'{ref}={v}' for ref, v in zip(rate_refs, original_rates)))
        cli('save'); cli('reboot'); connected()
        restored = cli('instances')
        assert restored == original_layout, (original_layout, restored)
        assert cli('get', *rate_refs)['typed_values'] == original_rates
        print('PASS original layout and rates restored')
    if args.report:
        Path(args.report).write_text(json.dumps({'info': original_info, 'original_layout': original_layout, 'original_rates': original_rates, 'diagnostics': diagnostics, 'restored': restored, 'pass': True}, indent=2))


if __name__ == '__main__':
    main()
