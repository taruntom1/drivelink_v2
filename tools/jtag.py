#!/usr/bin/env python3
"""Debug the firmware on a board over the ESP32-S3's built-in USB-JTAG.

It uses the native USB port (the console port) and never touches the protocol
UART. It never writes flash; flash with tools/bench_fw.py. validate.py never
runs it, only its tests (tools/tests/test_jtag.py).

  info               the firmware on the board and the ELF that matches it
  bt                 a backtrace of every task
  tasks              every task: priority, core, smallest free stack, position
  print EXPR...      evaluate GDB expressions
  break LOC          stop at LOC, dump, run on (--count, --timeout, --dump)
  watch EXPR         stop when the value changes, dump, run on (same options)
  server             keep OpenOCD running for GDB or VS Code until Ctrl+C or stop
  gdb                interactive GDB, on a running server or its own
  stop               let the board run and end a running server
  decode [FILE]      source lines for a panic's backtrace (FILE or stdin); no board

Every command that attaches halts both CPUs and lets them run again before it
ends, also after an error, a timeout or Ctrl+C. Halting is not harmless: see
docs/bench.md, "JTAG".

The ELF is the one whose SHA-256 the running app reports, searched in the
firmware/build* directories of every worktree. Without a match the command is
refused; --elf FILE adds a candidate and --allow-mismatch uses it anyway.

Exit codes: 0 ok, 1 failed, 2 refused (no matching ELF, or the JTAG port is
busy), 3 `break`/`watch` timed out, 130 Ctrl+C.
"""
import argparse
import contextlib
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

import validate

CONFIG = validate.ROOT / 'tools' / 'jtag' / 'openocd.cfg'
TASKS = validate.ROOT / 'tools' / 'jtag' / 'gdb_tasks.py'
OUT = validate.ROOT / 'build' / 'jtag'  # ignored by .gitignore (build/)
SESSION_ELF = OUT / 'firmware.elf'      # named by .vscode/launch.json
SESSION_INIT = OUT / 'init.gdb'         # the same
GDB_PORT, TCL_PORT = 23333, 23334       # as in openocd.cfg
APP_DESC_MAGIC = 0xABCD5432
PROJECT = 'drivelink_v2'
OK, FAILED, REFUSED, TIMEOUT, INTERRUPTED = 0, 1, 2, 3, 130

WARNING = ('WARNING: halting stops control and safety code while the PWM outputs keep their\n'
           '         last value. Cut motor power first, or DISARM. The watchdogs stay off\n'
           '         until the next reset: reboot the board before arming it.')


class ToolError(Exception):
    def __init__(self, message, code=FAILED):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class App:
    project: str
    version: str
    sha256: str


# --- The running app and its ELF -------------------------------------------

def parse_app_desc(words):
    """The App in an esp_app_desc_t given as its first 64 32-bit words."""
    data = b''.join(word.to_bytes(4, 'little') for word in words)
    if len(data) < 176 or words[0] != APP_DESC_MAGIC:
        raise ToolError('no app description on the board (is the firmware running?)')

    def text(start, size):
        return data[start:start + size].split(b'\0', 1)[0].decode('utf-8', 'replace')
    return App(project=text(48, 32), version=text(16, 32), sha256=data[144:176].hex())


def worktrees():
    done = subprocess.run(['git', 'worktree', 'list', '--porcelain'], cwd=validate.ROOT,
                          capture_output=True, text=True)
    found = [Path(line[9:]) for line in done.stdout.splitlines() if line.startswith('worktree ')]
    # This checkout first.
    return [validate.ROOT, *(path for path in found if path.resolve() != validate.ROOT)]


def candidates(extra=None):
    paths = [Path(extra)] if extra else []
    for tree in worktrees():
        paths += sorted((tree / 'firmware').glob(f'build*/{PROJECT}.elf'))
    return paths


def find_elf(sha_prefix, paths):
    """The first path whose SHA-256 starts with sha_prefix, or None."""
    for path in paths:
        with contextlib.suppress(OSError):
            if hashlib.sha256(path.read_bytes()).hexdigest().startswith(sha_prefix.lower()):
                return path
    return None


def choose_elf(app, args):
    if args.elf and not Path(args.elf).is_file():
        raise ToolError(f'{args.elf}: no such file', REFUSED)
    match = find_elf(app.sha256, candidates(args.elf))
    if match:
        return match
    message = (f'no ELF matches the firmware on the board ({app.project} {app.version}, '
               f'ELF SHA-256 {app.sha256[:16]})')
    if not args.allow_mismatch:
        raise ToolError(message + '. Build and flash it (tools/bench_fw.py), or give its ELF with '
                        '--elf; --allow-mismatch uses an ELF that does not match', REFUSED)
    if not args.elf:
        raise ToolError('--allow-mismatch needs --elf FILE', REFUSED)
    print(f'WARNING: {message}; using {args.elf}, so lines and values may be wrong', file=sys.stderr)
    return Path(args.elf)


# --- OpenOCD and GDB -------------------------------------------------------

def tool(env, name):
    path = shutil.which(name, path=env.get('PATH'))
    if not path:
        raise ToolError(f'{name} not found in the ESP-IDF environment (run install.ps1 or install.sh)')
    return path


def own_group():
    """Keep Ctrl+C away from a child, so that it ends through the shutdown hook."""
    return ({'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if validate.WINDOWS
            else {'start_new_session': True})


def listening(port=TCL_PORT):
    with contextlib.suppress(OSError), socket.create_connection(('127.0.0.1', port), timeout=0.5):
        return True
    return False


def tcl(command, timeout=10):
    """One command to a running OpenOCD; its answer."""
    with socket.create_connection(('127.0.0.1', TCL_PORT), timeout=timeout) as link:
        link.sendall(command.encode() + b'\x1a')
        data = b''
        while not data.endswith(b'\x1a'):
            part = link.recv(4096)
            if not part:
                break
            data += part
    return data.rstrip(b'\x1a').decode('utf-8', 'replace')


def openocd_error(text):
    """What to tell the user about an OpenOCD that did not come up."""
    if any(sign in text for sign in ('LIBUSB_ERROR_ACCESS', 'LIBUSB_ERROR_BUSY', 'claim')):
        return ToolError('the JTAG interface is in use by another program', REFUSED)
    if "couldn't bind" in text:
        return ToolError(f'port {GDB_PORT} or {TCL_PORT} is taken or reserved '
                         '(netsh interface ipv4 show excludedportrange tcp)')
    if 'could not find or open device' in text or 'device not found' in text.lower():
        return ToolError('no ESP32-S3 USB-JTAG device found (native USB cable and WinUSB driver?)')
    errors = [line for line in text.splitlines() if line.startswith('Error')]
    return ToolError('OpenOCD failed: ' + (errors[-1] if errors else 'see build/jtag/openocd.log'))


@contextlib.contextmanager
def openocd(env):
    """A running OpenOCD. On every exit the board runs again and OpenOCD ends."""
    if listening():
        raise ToolError('a JTAG session is open already: use `gdb`, or end it with `stop`', REFUSED)
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / 'openocd.log'
    with path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen([tool(env, 'openocd'), '-f', str(CONFIG)], cwd=OUT, env=env,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                   **own_group())
        started = False
        try:
            end = time.monotonic() + 15
            while not (listening(TCL_PORT) and 'Listening on port %d for gdb' % GDB_PORT
                       in path.read_text(encoding='utf-8', errors='replace')):
                if process.poll() is not None:
                    raise openocd_error(path.read_text(encoding='utf-8', errors='replace'))
                if time.monotonic() > end:
                    raise ToolError(f'OpenOCD did not start in 15 s; see {path}')
                time.sleep(0.1)
            started = True
            yield process
        finally:
            release(process, env, started)


def release(process, env, started=True):
    """End OpenOCD with the board running, whatever state it is in."""
    if process.poll() is None:
        with contextlib.suppress(OSError):
            tcl('shutdown')
        with contextlib.suppress(subprocess.TimeoutExpired):
            process.wait(10)
            return
        process.kill()
        process.wait()
        recover(env)
    elif started and process.returncode:
        recover(env)


def recover(env):
    """After an OpenOCD that died: attach again and let the board run."""
    print('OpenOCD did not end cleanly; resuming the board', file=sys.stderr)
    done = subprocess.run([tool(env, 'openocd'), '-f', str(CONFIG), '-c', 'init', '-c', 'halt 1000',
                           '-c', 'shutdown'], cwd=OUT, env=env, stdin=subprocess.DEVNULL,
                          capture_output=True, timeout=30)
    if done.returncode:
        print('Could not resume the board: reset it', file=sys.stderr)


def running_app():
    """Read the app description through a running OpenOCD. Halts for a moment."""
    tcl('halt 1000')
    try:
        words = tcl('drivelink_app_desc').split()
    finally:
        tcl('drivelink_release')
    try:
        return parse_app_desc([int(word, 0) for word in words])
    except (ValueError, IndexError):
        raise ToolError('no app description on the board (is the firmware running?)') from None


def gdb_init(elf):
    """GDB settings for an ELF: ROM and bootloader symbols when its build has them."""
    lines = ['set pagination off', 'set confirm off', 'set remotetimeout 15', 'set print pretty on']
    symbols = elf.parent / 'gdbinit' / 'symbols'
    if symbols.is_file():
        # Without its last line, which loads the app of that build directory.
        lines += [line for line in symbols.read_text(encoding='utf-8').splitlines()
                  if not line.startswith('file ')]
        lines.append('set confirm off')  # the file turns it on again
    return '\n'.join(lines) + '\n'


def prepare_session(elf):
    """Put the chosen ELF and its GDB settings where VS Code and GDB expect them."""
    OUT.mkdir(parents=True, exist_ok=True)
    SESSION_INIT.write_text(gdb_init(elf), encoding='utf-8')
    shutil.copyfile(elf, SESSION_ELF)


def gdb_command(env, *options):
    return [tool(env, 'xtensa-esp32s3-elf-gdb'), '-nx', '-q', *options, SESSION_ELF.as_posix(),
            '-x', SESSION_INIT.as_posix(), '-ex', f'target remote 127.0.0.1:{GDB_PORT}']


def run_script(env, commands, timeout):
    """Run GDB commands in batch mode. Returns GDB's exit code, or TIMEOUT."""
    script = OUT / 'commands.gdb'
    script.write_text('\n'.join(commands) + '\n', encoding='utf-8')
    process = subprocess.Popen([*gdb_command(env, '-batch'), '-x', script.as_posix()],
                               env=env, stdin=subprocess.DEVNULL, cwd=validate.ROOT, **own_group())
    try:
        return process.wait(timeout)
    except subprocess.TimeoutExpired:
        return TIMEOUT
    finally:
        if process.poll() is None:
            kill_tree(process)


def kill_tree(process):
    """End GDB: xtensa-esp32s3-elf-gdb is a wrapper that would leave the real GDB behind."""
    if validate.WINDOWS:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)], capture_output=True)
    else:
        os.killpg(process.pid, signal.SIGKILL)
    process.wait()


def prime(env):
    """One short GDB connection. OpenOCD finds FreeRTOS on the first connection and gives
    that one wrong thread names and registers; later connections get the right ones."""
    process = subprocess.Popen([*gdb_command(env, '-batch'), '-ex', 'detach'], env=env, cwd=validate.ROOT,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, **own_group())
    try:
        process.wait(30)
    except subprocess.TimeoutExpired:
        raise ToolError('GDB did not connect to OpenOCD') from None
    finally:
        if process.poll() is None:
            kill_tree(process)


# --- Commands --------------------------------------------------------------

def stop_script(setup, count, expressions):
    """Run until a stop `count` times, dumping each time."""
    body = ['continue', 'bt', 'info args', 'info locals',
            *(f'print {text}' for text in expressions), 'set $hit = $hit + 1']
    return [setup, 'set $hit = 0', f'while $hit < {count}', *(f'  {line}' for line in body), 'end', 'delete']


def script_for(args):
    """(GDB commands, time limit in seconds)"""
    if args.command == 'bt':
        return ['thread apply all bt'], 60
    if args.command == 'tasks':
        return [f'source {TASKS.as_posix()}'], 60
    if args.command == 'print':
        return [f'print {text}' for text in args.expr], 60
    if args.command == 'break':
        return stop_script(f'hbreak {args.location}', args.count, args.dump), args.timeout
    return stop_script(f'watch -location {args.expression}', args.count, args.dump), args.timeout


def one_shot(args, env):
    print(WARNING, file=sys.stderr, flush=True)
    with openocd(env):
        app = running_app()
        if args.command == 'info':
            match = find_elf(app.sha256, candidates(args.elf))
            print(f'project  {app.project}\nversion  {app.version}\nelf      {app.sha256}\n'
                  f'match    {match or "none"}')
            return OK
        prepare_session(choose_elf(app, args))
        prime(env)
        commands, limit = script_for(args)
        code = run_script(env, commands, limit)
    if code == TIMEOUT:
        print(f'TIMEOUT: not finished in {limit:g} s; the board is running again', file=sys.stderr)
        return TIMEOUT
    return OK if code == 0 else FAILED


def server(args, env):
    print(WARNING, file=sys.stderr, flush=True)
    with openocd(env) as process:
        prepare_session(choose_elf(running_app(), args))
        prime(env)
        print(f'READY: GDB on 127.0.0.1:{GDB_PORT}, ELF {SESSION_ELF}\n'
              'Ctrl+C or `jtag.py stop` ends it and lets the board run.', flush=True)
        while process.poll() is None:
            time.sleep(0.2)
    print('JTAG server ended; reboot the board before arming it')
    return FAILED if process.returncode else OK


def interactive(args, env):
    if listening():
        if not SESSION_ELF.is_file():
            raise ToolError('the open session was not started by this checkout', REFUSED)
        return run_interactive(env)
    print(WARNING, file=sys.stderr, flush=True)
    with openocd(env):
        prepare_session(choose_elf(running_app(), args))
        prime(env)
        return run_interactive(env)


def run_interactive(env):
    # Ctrl+C interrupts the program being debugged, not this script.
    previous = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        return OK if subprocess.run(gdb_command(env), env=env).returncode == 0 else FAILED
    finally:
        signal.signal(signal.SIGINT, previous)


def stop(args, env):
    if not listening():
        print('no JTAG session is open')
        return OK
    with contextlib.suppress(OSError):
        tcl('shutdown')
    end = time.monotonic() + 10
    while listening():
        if time.monotonic() > end:
            raise ToolError('the JTAG session did not end')
        time.sleep(0.1)
    print('JTAG session ended; the board is running. Reboot it before arming it')
    return OK


def panic_addresses(text):
    """The ELF SHA prefix in a panic log (or None) and each backtrace's PCs."""
    sha = re.search(r'ELF file SHA256:\s*([0-9a-fA-F]+)', text)
    traces = []
    for line in text.splitlines():
        found = re.search(r'Backtrace:((?:\s*0x[0-9a-fA-F]{8}:0x[0-9a-fA-F]{8})+)', line)
        if found:
            traces.append(re.findall(r'(0x[0-9a-fA-F]{8}):0x', found.group(1)))
    return (sha.group(1) if sha else None), traces


def decode(args, env):
    text = (Path(args.file).read_text(encoding='utf-8', errors='replace')
            if args.file and args.file != '-' else sys.stdin.read())
    sha, traces = panic_addresses(text)
    if not traces:
        raise ToolError('no "Backtrace:" line in the input')
    elf = find_elf(sha, candidates(args.elf)) if sha else None
    if elf is None:
        if not (args.elf and args.allow_mismatch):
            reason = (f'no ELF matches the panic (ELF SHA-256 {sha})' if sha
                      else 'the input has no "ELF file SHA256" line')
            raise ToolError(f'{reason}; give one with --elf FILE --allow-mismatch', REFUSED)
        elf = Path(args.elf)
        print(f'WARNING: {elf} is not known to match the panic', file=sys.stderr)
    print(f'ELF {elf}')
    for trace in traces:
        done = subprocess.run([tool(env, 'xtensa-esp32s3-elf-addr2line'), '-pfiaC', '-e', str(elf), *trace],
                              env=env, capture_output=True, text=True, errors='replace')
        if done.returncode:
            raise ToolError(f'addr2line failed: {done.stderr.strip()}')
        print(f'Backtrace:\n{done.stdout.rstrip()}')
    return OK


def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError('must be at least 1')
    return value


def positive_float(text):
    value = float(text)
    if not 0 < value < float('inf'):
        raise argparse.ArgumentTypeError('must be more than 0')
    return value


def one_line(text):
    """One argument is one GDB command line."""
    if '\n' in text or '\r' in text:
        raise argparse.ArgumentTypeError('must be one line')
    return text


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    def add(name):
        command = sub.add_parser(name)
        command.add_argument('--elf', help='an ELF to consider first')
        command.add_argument('--allow-mismatch', action='store_true', help='use --elf though it does not match')
        return command
    for name in ('info', 'bt', 'tasks', 'server', 'gdb'):
        add(name)
    sub.add_parser('stop')
    add('print').add_argument('expr', nargs='+', metavar='EXPR', type=one_line)
    for name, operand in (('break', 'location'), ('watch', 'expression')):
        command = add(name)
        command.add_argument(operand, type=one_line)
        command.add_argument('--count', type=positive_int, default=1, help='stops before it ends (default 1)')
        command.add_argument('--timeout', type=positive_float, default=30, help='seconds (default 30)')
        command.add_argument('--dump', action='append', default=[], metavar='EXPR', type=one_line,
                             help='also print this at each stop')
    add('decode').add_argument('file', nargs='?', help='a console log; stdin by default')
    return parser.parse_args(argv)


def interrupted(number, frame):
    raise KeyboardInterrupt


COMMANDS = {'server': server, 'gdb': interactive, 'stop': stop, 'decode': decode}


def main(argv=None):
    args = arguments(argv)
    # Ctrl+Break and a polite kill also leave through the cleanup.
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), interrupted)
    try:
        env = None if args.command == 'stop' else validate.idf_env()
        return COMMANDS.get(args.command, one_shot)(args, env)
    except ToolError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return error.code
    except (validate.Unavailable, OSError, subprocess.SubprocessError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return FAILED
    except KeyboardInterrupt:
        return INTERRUPTED


if __name__ == '__main__':
    sys.exit(main())
