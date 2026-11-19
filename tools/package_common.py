"""What tools/package_win.py and tools/package_linux.py share: errors, command
runs and the smoke of the staged programs (CLI --help, vdev answering the CLI,
the GUI rendering the Connect screen offscreen). Standard library only.
"""
import json
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time

SCREENSHOT = 'connect-2560x1440-dark.png'


class Failure(Exception):
    """A step failed: exit 1."""


class Unavailable(Exception):
    """A prerequisite is missing: exit 2."""


def say(text):
    print(text, flush=True)


def run(argv, *, env=None, cwd=None, timeout=None, log=None):
    """Run a command; return the CompletedProcess. A log file collects stdout and stderr."""
    try:
        if log:
            with open(log, 'w', encoding='utf-8', errors='replace') as sink:
                return subprocess.run([str(a) for a in argv], env=env, cwd=cwd, timeout=timeout,
                                      stdout=sink, stderr=subprocess.STDOUT)
        return subprocess.run([str(a) for a in argv], env=env, cwd=cwd, timeout=timeout,
                              capture_output=True, text=True, errors='replace')
    except subprocess.TimeoutExpired as error:
        raise Failure(f'{Path(str(argv[0])).name} timed out after {timeout} s') from error
    except OSError as error:
        raise Failure(f'cannot run {argv[0]}: {error}') from error


def run_logged(argv, log, **kwargs):
    """Run with a log file; on failure raise with the log's last lines."""
    done = run(argv, log=log, **kwargs)
    if done.returncode != 0:
        tail = '\n'.join(Path(log).read_text(encoding='utf-8', errors='replace').splitlines()[-15:])
        raise Failure(f'{" ".join(str(a) for a in argv[:2])} failed (exit {done.returncode}), '
                      f'see {log}\n{tail}')
    return done


def read_lines(stream, lines):
    for line in stream:
        lines.put(line.rstrip())
    lines.put(None)


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def start_vdev(vdev, env):
    """Start vdev on an ephemeral port; return (process, endpoint)."""
    process = subprocess.Popen([str(vdev), '--port', '0'], env=env,
                               cwd=tempfile.gettempdir(), stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors='replace')
    lines = queue.Queue()
    threading.Thread(target=read_lines, args=(process.stdout, lines), daemon=True).start()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=0.5)
        except queue.Empty:
            continue
        if line is None:
            break
        if line.startswith('READY '):
            return process, line.split(None, 1)[1]
    stop(process)
    raise Failure('drivelink-vdev did not print READY')


def smoke(bin_dir, env, suffix=''):
    """Run the staged drivelink-{cli,vdev,gui}<suffix> in bin_dir from a scratch directory."""
    bin_dir = Path(bin_dir)
    program = {name: str(bin_dir / f'drivelink-{name}{suffix}') for name in ('cli', 'vdev', 'gui')}
    with tempfile.TemporaryDirectory(prefix='drivelink-smoke-') as scratch:
        done = run([program['cli'], '--help'], env=env, cwd=scratch, timeout=30)
        if done.returncode != 0:
            raise Failure(f'drivelink-cli --help exited {done.returncode}')
        say('smoke: cli --help ok')
        process, endpoint = start_vdev(program['vdev'], env)
        try:
            done = run([program['cli'], '--endpoint', endpoint, '--json', 'info'], env=env,
                       cwd=scratch, timeout=30)
            if done.returncode != 0:
                raise Failure(f'drivelink-cli info exited {done.returncode}: {done.stderr.strip()}')
            try:
                firmware = json.loads(done.stdout)['data']['firmware_version']
            except (ValueError, KeyError, TypeError) as error:
                raise Failure(f'drivelink-cli info gave no data.firmware_version: {error}') from error
            if not str(firmware).endswith('-vdev'):
                raise Failure(f'vdev reported firmware_version {firmware!r}, expected *-vdev')
        finally:
            stop(process)
        say(f'smoke: vdev answered the cli ({firmware})')
        shots = Path(scratch) / 'shots'
        done = run([program['gui'], '--screen', 'connect', '--out', shots],
                   env=env, cwd=scratch, timeout=60)
        if done.returncode != 0:
            raise Failure(f'drivelink-gui --screen connect exited {done.returncode}: '
                          f'{done.stderr.strip()[-800:]}')
        if not (shots / SCREENSHOT).is_file() or (shots / SCREENSHOT).stat().st_size == 0:
            raise Failure(f'drivelink-gui wrote no {SCREENSHOT}')
        say('smoke: gui rendered connect')
