"""Exercise the public snapshot consumer against an independent real-core vdev."""
import argparse
import json
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--vdev', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='drivelink-model-') as state:
        process = None

        def start():
            nonlocal process, endpoint
            process = subprocess.Popen([args.vdev, '--port', '0', '--state', state],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            line = process.stdout.readline().strip()
            assert line.startswith('READY '), (line, process.poll())
            endpoint = line[6:]

        def stop():
            nonlocal process
            if process is not None:
                process.kill()
                process.communicate(timeout=10)
                process = None

        def cli(*command):
            result = subprocess.run([args.cli, '--endpoint', endpoint, '--json', *command],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (command, result.stdout, result.stderr)
            return json.loads(result.stdout)['data']

        def snapshot():
            result = subprocess.run([args.probe, endpoint], capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, (result.stdout, result.stderr)
            rows = [line.split('\t') for line in result.stdout.splitlines()]
            instances = {(r[1], int(r[2]), int(r[3])): (int(r[4]), int(r[5]), r[6])
                         for r in rows if r[0] == 'INSTANCE'}
            params = {(r[1], int(r[2]), int(r[3]), int(r[4])): int(r[5])
                      for r in rows if r[0] == 'PARAM'}
            system = {int(r[1]): int(r[2]) for r in rows if r[0] == 'SYSTEM'}
            assert all(id_ in system for id_ in range(16, 23)), system
            assert next(int(r[1]) for r in rows if r[0] == 'CODES') > 0
            assert any(r == ['ENUM', '5', '3', '2', 'any'] for r in rows), rows
            return rows, instances, params

        endpoint = ''
        try:
            start()
            rows, instances, params = snapshot()
            assert len(instances) == 4 and params['CONFIG', 5, 0, 1] == 1000
            # Keep a hole in the loop IDs and a pending-only instance.
            unused = cli('add', '5', 'hole')['instance']
            extra = cli('add', '5', 'sparse')['instance']
            cli('remove', '5', str(unused))
            cli('set', f'5:{extra}:1=125', f'5:{extra}:2=1', f'5:{extra}:3=2')
            motor = cli('add', '112', 'pending', str(extra))['instance']
            rows, instances, params = snapshot()
            assert ('CONFIG', 5, extra) not in instances
            assert ('PENDING', 5, unused) not in instances
            assert instances['PENDING', 112, motor] == (extra, 255, 'pending')
            assert params['PENDING', 5, extra, 1] == 125
            cli('save'); cli('reboot')
            rows, instances, params = snapshot()
            assert instances['CONFIG', 112, motor] == (extra, 0, 'pending')
            assert params['CONFIG', 5, extra, 1] == 125
            # One view removes an instance, another stages a label and placement.
            cli('label', '5', str(extra), 'renamed')
            cli('set', f'5:{extra}:3=0')
            cli('remove', '112', str(motor))
            rows, instances, params = snapshot()
            assert ('CONFIG', 112, motor) in instances and ('PENDING', 112, motor) not in instances
            assert instances['CONFIG', 5, extra][2] == 'sparse'
            assert instances['PENDING', 5, extra][2] == 'renamed'
            assert params['CONFIG', 5, extra, 3] == 2 and params['PENDING', 5, extra, 3] == 0
            cli('discard')
            # Native crash persistence drives real safe mode; no new vdev hook.
            for _ in range(3):
                stop(); start()
            rows, instances, params = snapshot()
            assert int(next(r[1] for r in rows if r[0] == 'INFO')) & 8
            assert all(value[1] == 4 for key, value in instances.items() if key[0] == 'CONFIG')
            assert params['CONFIG', 5, extra, 1] == 125
            print('PASS device snapshots: discovery, limits, enums, sparse/pending layouts, reload and safe mode')
        finally:
            stop()


if __name__ == '__main__':
    main()
