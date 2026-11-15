"""Run the production async consumer on a fresh real-core vdev."""
import argparse
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    parser.add_argument('--vdev', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='drivelink-async-') as state:
        process = subprocess.Popen([args.vdev, '--port', '0', '--state', state],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = process.stdout.readline().strip()
            assert line.startswith('READY '), (line, process.poll())
            subprocess.run([args.probe, line[6:]], check=True, timeout=35)
        finally:
            process.kill()
            process.communicate(timeout=10)


if __name__ == '__main__':
    main()
