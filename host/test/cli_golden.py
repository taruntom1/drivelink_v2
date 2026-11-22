"""CLI compatibility fixtures captured before H1a-1; no hardware.

Only firmware/build identity and diagnostic uptime vary in these scenarios.
JSON preserves all keys/types; readable output preserves whitespace/order.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

COMMANDS = [
    ['info'], ['types'], ['items', '0'], ['items', '5'], ['items', '112'],
    ['enums', '5', '4'], ['codes'], ['instances'], ['instances', 'pending'],
    ['groups'], ['get', '0:0:4'], ['capture-status'], ['testgen-status', '0'],
    ['ota-status'], ['diag-info'], ['diag-entries'], ['diag-clear'],
    ['add', '112', 'golden'], ['instances', 'pending'], ['label', '112', '0', 'renamed'],
    ['remove', '112', '0'], ['discard'], ['revert'], ['disarm'],
    ['set', '0:0:4=200'], ['get', '0:0:4'],
    ['get', '0:0:65535'], ['set', '0:0:4=oops'], ['arm'], ['unknown-command'],
]


def normalize(text, json_mode):
    if json_mode:
        value = json.loads(text)

        def visit(node):
            if isinstance(node, dict):
                for key in list(node):
                    if key in ('build_id', 'elf_sha256', 'firmware_version'):
                        node[key] = '<identity>'
                    elif key == 'uptime_us':
                        node[key] = 0
                    else:
                        visit(node[key])
            elif isinstance(node, list):
                for child in node:
                    visit(child)
        visit(value)
        return value
    text = re.sub(r'(?m)^(\s*(?:build_id|elf_sha256|firmware_version): ).*$', r'\1<identity>', text)
    return re.sub(r'(?m)^(\s*uptime_us: )\d+$', r'\g<1>0', text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--vdev', required=True)
    ap.add_argument('--record', action='store_true', help='baseline capture only; never update during a refactor')
    args = ap.parse_args()
    fixture = Path(__file__).with_name('fixtures') / 'cli-golden.json'
    actual = []
    for json_mode in (False, True):
        with tempfile.TemporaryDirectory(prefix='drivelink-golden-') as state:
            process = subprocess.Popen([args.vdev, '--port', '0', '--state', state],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                ready = process.stdout.readline().strip()
                assert ready.startswith('READY '), ready
                for command in COMMANDS:
                    run = subprocess.run([args.cli, '--endpoint', ready[6:], *(['--json'] if json_mode else []), *command],
                                         capture_output=True, text=True, timeout=20)
                    actual.append(dict(command=command, json=json_mode, code=run.returncode,
                                       stdout=normalize(run.stdout, json_mode) if run.stdout else '', stderr=run.stderr))
            finally:
                process.kill()
                process.communicate(timeout=10)
    if args.record:
        fixture.parent.mkdir(exist_ok=True)
        fixture.write_text(json.dumps(actual, indent=2) + '\n', encoding='utf-8')
    else:
        expected = json.loads(fixture.read_text(encoding='utf-8'))
        for before, after in zip(expected, actual, strict=True):
            assert before == after, (before, after)
    print(f'PASS {len(actual)} CLI golden outputs (readable and JSON)')


if __name__ == '__main__':
    main()
