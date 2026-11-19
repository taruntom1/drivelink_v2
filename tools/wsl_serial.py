#!/usr/bin/env python3
"""Lend the bench board's CH343 USB serial adapter to WSL, and always take it back.

`drivelink-agent` is Linux only, so hardware tests that need it attach the
board's serial adapter (1a86:55d3) to WSL through usbipd-win. Windows owns the
adapter again after `detach` or `run`; `bench_fw.py` and the host need it there.

One-time setup, in an admin shell: `usbipd bind --busid <id>`. This tool never
binds, unbinds or needs admin. It touches hardware, so `validate.py` never runs
it; only its tests (tools/tests/test_wsl_serial.py) are part of validation.

  status            show the matching adapters and their state
  attach            attach to WSL and print the Linux tty (refuses unless bound)
  detach            give it back to Windows and wait until it is back
  run [--] CMD...   attach, run CMD with DRIVELINK_WSL_TTY set (and passed into
                    wsl.exe through WSLENV), then always detach, also on Ctrl+C
                    or when CMD fails

Exit codes: 0 ok, 1 failed (usbipd error, tty never appeared), 2 refused (not
found, ambiguous, not bound, usbipd missing), 3 `status` found no adapter,
4 detach failed (the adapter may still be in WSL). `run` returns CMD's exit
code (130 after Ctrl+C) unless the detach failed.

--dry-run prints the usbipd/wsl commands that would change something and runs
none of them. It still reads `usbipd state`; if usbipd cannot run and --busid is
given it assumes that device is bound and on Windows, otherwise it refuses.
"""
import argparse
from dataclasses import dataclass
import json
import os
import re
import shutil
import subprocess
import sys
import time


VID_PID = '1a86:55d3'
USBIPD = r'C:\Program Files\usbipd-win\usbipd.exe'
TTY_GLOBS = ('/dev/ttyACM*', '/dev/ttyUSB*')
ENV_TTY = 'DRIVELINK_WSL_TTY'
OK, FAILED, REFUSED, NONE, DETACH_FAILED, INTERRUPTED = 0, 1, 2, 3, 4, 130
LABELS = {'not-connected': 'not connected', 'not-shared': 'not shared (not bound)',
          'shared': 'shared, on Windows', 'attached': 'attached to WSL'}


class ToolError(Exception):
    def __init__(self, message, code=REFUSED, missing=False):
        super().__init__(message)
        self.code = code
        self.missing = missing  # nothing matched (as opposed to ambiguous)


@dataclass(frozen=True)
class Device:
    busid: str | None      # None: bound, but not plugged in
    description: str
    vid_pid: str
    bound: bool            # PersistedGuid present: `usbipd bind` was done
    client: str | None     # ClientIPAddress: attached to a client (WSL)
    assumed: bool = False  # invented for a dry run

    @property
    def state(self):
        if self.busid is None:
            return 'not-connected'
        if self.client:
            return 'attached'
        return 'shared' if self.bound else 'not-shared'


@dataclass
class Context:
    usbipd: str = USBIPD
    distro: str | None = None
    timeout: float = 10.0
    dry_run: bool = False
    vid_pid: str = VID_PID
    busid: str | None = None

    def wsl(self, *args):
        distro = ['-d', self.distro] if self.distro else []
        return [shutil.which('wsl.exe') or 'wsl.exe', *distro, '-e', *args]


def parse_state(text):
    """Devices from `usbipd state` JSON (the output may have a BOM or a banner)."""
    data = json.loads(text[text.index('{'):])
    devices = []
    for item in data.get('Devices') or []:
        match = re.search(r'VID_([0-9A-F]{4})&PID_([0-9A-F]{4})', item.get('InstanceId') or '', re.I)
        devices.append(Device(item.get('BusId'), item.get('Description') or '',
                              f'{match[1]}:{match[2]}'.lower() if match else '',
                              bool(item.get('PersistedGuid')), item.get('ClientIPAddress')))
    return devices


def select(devices, vid_pid, busid=None, prefer_attached=False):
    """The one connected device to act on; ToolError if none or several match."""
    if busid:
        found = [d for d in devices if d.busid == busid]
        if not found:
            raise ToolError(f'no connected USB device with busid {busid} (see `usbipd list`)', missing=True)
        return found[0]
    found = [d for d in devices if d.vid_pid == vid_pid and d.busid]
    if prefer_attached and len(found) > 1 and sum(d.state == 'attached' for d in found) == 1:
        found = [d for d in found if d.state == 'attached']
    if not found:
        raise ToolError(f'no connected device {vid_pid}: plug the board in', missing=True)
    if len(found) > 1:
        names = ', '.join(f'{d.busid} ({d.description})' for d in found)
        raise ToolError(f'{len(found)} devices match {vid_pid}: {names}; pass --busid')
    return found[0]


def capture(argv, timeout=30):
    """Run argv; returns (exit code, stdout, stderr). OSError if it cannot start."""
    try:
        done = subprocess.run(argv, capture_output=True, timeout=timeout, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return 124, '', f'timed out after {timeout} s'
    return (done.returncode, done.stdout.decode('utf-8-sig', 'replace'),
            done.stderr.decode('utf-8', 'replace'))


def read_devices(ctx):
    try:
        code, out, err = capture([ctx.usbipd, 'state'])
    except OSError as error:
        raise ToolError(f'cannot run usbipd ({error}); install usbipd-win or pass --usbipd')
    if code:
        raise ToolError(f'usbipd state failed: {err.strip() or out.strip()}', FAILED)
    try:
        return parse_state(out)
    except ValueError as error:  # includes json.JSONDecodeError
        raise ToolError(f'cannot parse usbipd state output: {error}', FAILED)


def locate(ctx, prefer_attached=False):
    try:
        devices = read_devices(ctx)
    except ToolError:
        if not (ctx.dry_run and ctx.busid):
            raise
        print(f'DRY RUN: usbipd state unavailable; assuming {ctx.busid} is bound and on Windows')
        return Device(ctx.busid, '(assumed)', ctx.vid_pid, True, None, assumed=True)
    return select(devices, ctx.vid_pid, ctx.busid, prefer_attached)


def wsl_ttys(ctx):
    """Serial devices in WSL (best effort: empty when WSL cannot be asked)."""
    try:
        _, out, _ = capture(ctx.wsl('sh', '-c', f'ls {" ".join(TTY_GLOBS)} 2>/dev/null'))
    except OSError:
        return []
    return sorted({line.strip() for line in out.splitlines() if line.strip().startswith('/dev/')})


def poll(check, timeout, interval=0.5):
    """First truthy check() within timeout seconds, else None."""
    deadline = time.monotonic() + timeout
    while True:
        result = check()
        if result:
            return result
        if time.monotonic() >= deadline:
            return None
        time.sleep(interval)


def show(argv):
    print('DRY RUN: ' + subprocess.list2cmdline(argv))


def check_attachable(dev):
    if dev.state == 'not-shared':
        raise ToolError(f'{dev.busid} ({dev.description}) is not shared. Once, in an admin shell, run:\n'
                        f'    usbipd bind --busid {dev.busid}')


def attach_device(ctx, dev):
    """Attach dev to WSL; returns its Linux tty (a placeholder in a dry run)."""
    check_attachable(dev)
    placeholder = '<tty in WSL>'
    wait = f'wait up to {ctx.timeout:g} s for {" or ".join(TTY_GLOBS)} in WSL'
    if dev.state == 'attached':
        print(f'{dev.busid} is already attached to WSL')
        if ctx.dry_run:
            return placeholder
        before = set()
    else:
        argv = [ctx.usbipd, 'attach', '--busid', dev.busid, '--wsl', *([ctx.distro] if ctx.distro else [])]
        if ctx.dry_run:
            show(argv)
            print('DRY RUN: ' + wait)
            return placeholder
        before = set(wsl_ttys(ctx))
        try:
            code, out, err = capture(argv)
        except OSError as error:
            raise ToolError(f'cannot run usbipd: {error}', FAILED)
        if code:
            raise ToolError(f'usbipd attach failed ({code}): {err.strip() or out.strip()}', FAILED)
    tty = poll(lambda: next(iter(sorted(set(wsl_ttys(ctx)) - before)), None), ctx.timeout)
    if not tty:
        raise ToolError(f'{dev.busid} is attached but no {" or ".join(TTY_GLOBS)} appeared in WSL within '
                        f'{ctx.timeout:g} s (does its kernel have a CH343/cdc-acm driver?)', FAILED)
    print(f'{dev.busid} attached to WSL as {tty}')
    return tty


def attached_now(ctx, busid):
    """True/False for the device's state, None when `usbipd state` cannot be read."""
    try:
        return any(d.busid == busid and d.client for d in read_devices(ctx))
    except ToolError:
        return None


def detach_device(ctx, busid):
    """Detach busid and wait until Windows has it back; returns the exit code."""
    argv = [ctx.usbipd, 'detach', '--busid', busid]
    if ctx.dry_run:
        show(argv)
        print(f'DRY RUN: wait up to {ctx.timeout:g} s for {busid} to be back on Windows')
        return OK
    try:
        code, out, err = capture(argv)
    except OSError as error:
        print(f'ERROR: cannot run usbipd detach: {error}', file=sys.stderr)
        return DETACH_FAILED
    if code:  # the device may have gone already: the state below decides
        print(f'usbipd detach exited {code}: {err.strip() or out.strip()}', file=sys.stderr)
    if poll(lambda: attached_now(ctx, busid) is False, ctx.timeout):
        print(f'{busid} detached, back on Windows')
        return OK
    print(f'ERROR: {busid} is still attached to WSL after {ctx.timeout:g} s; '
          f'run: usbipd detach --busid {busid}', file=sys.stderr)
    return DETACH_FAILED


def detach_always(ctx, busid):
    """detach_device that a Ctrl+C cannot skip (up to three interrupts)."""
    for _ in range(3):
        try:
            return detach_device(ctx, busid)
        except KeyboardInterrupt:
            print('interrupted; detaching first', file=sys.stderr)
    return DETACH_FAILED


def cmd_status(ctx, as_json=False):
    devices = [d for d in read_devices(ctx) if (d.busid == ctx.busid if ctx.busid else d.vid_pid == ctx.vid_pid)]
    attached = [d for d in devices if d.state == 'attached']
    ttys = wsl_ttys(ctx) if attached else None  # asking starts WSL: only when it can matter
    found = any(d.busid for d in devices)
    if as_json:
        print(json.dumps({'found': found, 'wsl_ttys': ttys, 'devices': [
            {'busid': d.busid, 'description': d.description, 'vid_pid': d.vid_pid, 'state': d.state}
            for d in devices]}, indent=2))
    else:
        for d in devices:
            print(f'{d.busid or "-":<5} {d.description}  [{d.vid_pid}]  {LABELS[d.state]}')
        if not devices:
            print(f'no device {ctx.busid or ctx.vid_pid} found')
        if attached:
            print('WSL tty: ' + (', '.join(ttys) or 'none appeared'))
    return OK if found else NONE


def cmd_attach(ctx):
    attach_device(ctx, locate(ctx))
    return OK


def cmd_detach(ctx):
    try:
        dev = locate(ctx, prefer_attached=True)
    except ToolError as error:
        if not error.missing:
            raise
        print(f'{error}; nothing to detach')
        return OK
    if dev.state != 'attached' and not dev.assumed:
        print(f'{dev.busid} is not attached to WSL; nothing to do')
        return OK
    return detach_device(ctx, dev.busid)


def cmd_run(ctx, command):
    dev = locate(ctx)
    check_attachable(dev)  # refusals happen before anything is attached
    code = FAILED
    try:
        try:
            tty = attach_device(ctx, dev)
            if ctx.dry_run:
                print(f'DRY RUN: {ENV_TTY}={tty} {subprocess.list2cmdline(command)}')
                code = OK
            else:
                env = {**os.environ, ENV_TTY: tty,
                       'WSLENV': ':'.join(filter(None, [os.environ.get('WSLENV'), ENV_TTY]))}
                code = subprocess.call(command, env=env)
        except ToolError as error:
            print(f'ERROR: {error}', file=sys.stderr)
            code = error.code
        except OSError as error:
            print(f'ERROR: cannot run {command[0]}: {error}', file=sys.stderr)
            code = 127
        except KeyboardInterrupt:
            print('interrupted', file=sys.stderr)
            code = INTERRUPTED
    finally:  # the guarantee: whatever happened above, the adapter goes back to Windows
        detached = detach_always(ctx, dev.busid)
    return detached or code


def find_usbipd(override):
    return override or shutil.which('usbipd') or (USBIPD if os.path.exists(USBIPD) else 'usbipd')


def parse_args(argv):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--busid', help='act on this bus id (default: the one device matching --vid-pid)')
    common.add_argument('--vid-pid', default=VID_PID, help=f'adapter to look for (default: {VID_PID})')
    common.add_argument('--distro', help='WSL distribution (default: the WSL default)')
    common.add_argument('--timeout', type=float, default=10.0,
                        help='seconds to wait for the tty / for the detach (default: 10)')
    common.add_argument('--dry-run', action='store_true', help='print the commands that would run; change nothing')
    common.add_argument('--usbipd', help=f'usbipd.exe path (default: PATH, then {USBIPD})')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='mode', required=True)
    status = sub.add_parser('status', parents=[common], help='show the adapter(s) and their state')
    status.add_argument('--json', action='store_true', help='machine-readable output')
    sub.add_parser('attach', parents=[common], help='attach to WSL and print its tty')
    sub.add_parser('detach', parents=[common], help='give it back to Windows')
    run = sub.add_parser('run', parents=[common], help='attach, run CMD with DRIVELINK_WSL_TTY, always detach',
                         usage='wsl_serial.py run [options] [--] CMD...')
    run.add_argument('command', nargs=argparse.REMAINDER, help='command (options go before it)')
    args = parser.parse_args(argv)
    if args.mode == 'run':
        if args.command[:1] == ['--']:
            args.command = args.command[1:]
        if not args.command:
            parser.error('run needs a command')
    return args


def main(argv=None):
    args = parse_args(argv)
    ctx = Context(find_usbipd(args.usbipd), args.distro, args.timeout, args.dry_run,
                  args.vid_pid.lower(), args.busid)
    try:
        if args.mode == 'status':
            return cmd_status(ctx, args.json)
        if args.mode == 'attach':
            return cmd_attach(ctx)
        if args.mode == 'detach':
            return cmd_detach(ctx)
        return cmd_run(ctx, args.command)
    except ToolError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return error.code
    except KeyboardInterrupt:
        print('interrupted', file=sys.stderr)
        return INTERRUPTED


if __name__ == '__main__':
    sys.exit(main())
