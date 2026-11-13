#!/usr/bin/env python3
"""Run DriveLink's validation checks: one list, used locally and by CI.

    python tools/validate.py quick          # generator, native tests, this OS's host tests
    python tools/validate.py full           # every check below: the gate before a PR
    python tools/validate.py --only fw-nolog
    python tools/validate.py --list

Checks run one at a time, so a build never competes with another build or
with a timing-sensitive test. Each check sets up its own toolchain. Logs and
the summary go to build/validate/. A check that passed is not run again while
the code it covers is unchanged (--force runs it anyway).

No check flashes a board or opens a serial port. Hardware tests stay separate
(firmware/CLAUDE.md, "Hardware-in-the-loop and smoke tests").

Standard library only. The guide is in firmware/test/CLAUDE.md ("Validation").
"""
import argparse
import dataclasses
import functools
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'validate'  # ignored by .gitignore (build/)
WINDOWS = os.name == 'nt'
CI = bool(os.environ.get('CI') or os.environ.get('GITHUB_ACTIONS'))
GROUPS = bool(os.environ.get('GITHUB_ACTIONS'))

# This machine's toolchains, as firmware/CLAUDE.md and host/README.md give them.
# The windows-mingw preset and install_smoke.py name the same Qt compiler.
QT_TOOLS = Path('C:/Qt/Tools')
QT_DIRS = [QT_TOOLS / 'mingw1310_64/bin', QT_TOOLS / 'CMake_64/bin', QT_TOOLS / 'Ninja']
IDF_DEFAULT = Path.home() / 'esp/v5.5.2/esp-idf'
IDF_PYTHON_311 = Path('C:/Espressif/tools/idf-python/3.11.2')

# Paths that no check reads, and paths that only `gen` reads.
NOT_CODE = ['plans', '.claude', '.agents', '.github']
DOCS = [':(exclude)docs', ':(exclude,glob)**/*.md']


class Unavailable(Exception):
    """A prerequisite of the check is missing on this machine."""


class Failed(Exception):
    def __init__(self, message, detail='', warned=False):
        super().__init__(message)
        self.detail = detail
        self.warned = warned


@dataclasses.dataclass
class Step:
    title: str
    argv: list
    cwd: Path = ROOT
    env: dict = None
    warning_free: bool = False  # fail on any "warning:" in the output, like CI


@dataclasses.dataclass
class Check:
    name: str
    about: str
    plan: object                 # (jobs) -> [Step]; raises Unavailable
    windows_only: bool = False
    reads_docs: bool = False
    build_dir: Path = None       # removed before a run that follows a warning


# --- Toolchains ------------------------------------------------------------

def need(*tools, path=None):
    missing = [tool for tool in tools if not shutil.which(tool, path=path)]
    if missing:
        raise Unavailable('not on PATH: ' + ', '.join(missing))


def need_yaml():
    try:
        import yaml  # noqa: F401  (the generator needs it)
    except ImportError:
        raise Unavailable(f'PyYAML is missing for {sys.executable}') from None


def python():
    return Path(sys.executable).as_posix()


def parse_env(text, marker):
    """The NAME=VALUE lines after `marker`. Names are upper case, as os.environ has them."""
    env = {}
    for line in text.split(marker, 1)[1].splitlines():
        name, sep, value = line.partition('=')
        if sep and name:
            env[name.upper()] = value
    return env


@functools.cache
def idf_env():
    """The exported ESP-IDF environment: the caller's, or export.ps1's on Windows."""
    env = dict(os.environ)
    if env.get('IDF_PATH') and env.get('IDF_PYTHON_ENV_PATH'):
        return env  # exported already: CI, or an IDF shell
    if not WINDOWS:
        raise Unavailable('ESP-IDF is not exported (run `. $IDF_PATH/export.sh` first)')
    export = Path(env.get('IDF_PATH') or IDF_DEFAULT) / 'export.ps1'
    shell = shutil.which('pwsh') or shutil.which('powershell')
    if not export.is_file() or not shell:
        raise Unavailable(f'{export} or PowerShell not found (set IDF_PATH)')
    # The two fixes of firmware/CLAUDE.md ("Commands"): a tools path that has
    # the Python env, and Python 3.11 first on PATH.
    if not (Path(env.get('IDF_TOOLS_PATH', '')) / 'python_env').is_dir():
        env['IDF_TOOLS_PATH'] = str(Path.home() / '.espressif')
    if IDF_PYTHON_311.is_dir():
        env['PATH'] = f"{IDF_PYTHON_311};{env['PATH']}"
    env.pop('MSYSTEM', None)  # set by Git Bash; ESP-IDF refuses to run under MSYS
    marker = '---validate-env---'
    script = (f". '{export}' *> $null; '{marker}'; "
              "Get-ChildItem Env: | ForEach-Object { $_.Name + '=' + $_.Value }")
    done = subprocess.run([shell, '-NoProfile', '-NonInteractive', '-Command', script],
                          env=env, capture_output=True, text=True, errors='replace')
    exported = parse_env(done.stdout, marker) if marker in done.stdout else {}
    if not exported.get('IDF_PYTHON_ENV_PATH'):
        errors = [line for line in (done.stdout + done.stderr).splitlines() if line.startswith('ERROR')]
        raise Unavailable('ESP-IDF export failed' + (f' ({errors[0]})' if errors else '')
                          + '; see firmware/CLAUDE.md ("Commands")')
    return exported


def idf_build(cwd, *options):
    def plan(jobs):
        env = idf_env()
        idf_py = str(Path(env['IDF_PATH']) / 'tools' / 'idf.py')
        # idf.py has no job option for Ninja; the build runs alone instead.
        return [Step('idf.py build', ['python', idf_py, *options, 'build'],
                     cwd=ROOT / cwd, env=env, warning_free=True)]
    return plan


def plan_gen(jobs):
    need_yaml()
    return [
        Step('generated files are current', [python(), 'tools/gen/gen.py', '--check']),
        Step('generator tests', [python(), '-m', 'unittest', 'discover', '-s', 'tools/gen/tests']),
        Step('validate.py tests', [python(), '-m', 'unittest', 'discover', '-s', 'tools/tests']),
        Step('Python frame reference', [python(), 'firmware/test/uart_comm/protocol.py']),
    ]


def plan_native(jobs):
    need_yaml()
    need('cmake', 'ninja', 'g++')
    build = 'firmware/test/native/build'
    return [
        Step('configure', ['cmake', '-S', 'firmware/test/native', '-B', build, '-G', 'Ninja',
                           f'-DPython3_EXECUTABLE={python()}']),
        Step('build', ['cmake', '--build', build, '--parallel', jobs]),
        Step('test', ['ctest', '--test-dir', build, '--output-on-failure']),
    ]


def plan_host(preset, configure_preset, jobs, env=None):
    superbuild = ROOT / 'host/superbuild'
    return [
        Step('configure', ['cmake', '--preset', configure_preset], superbuild, env),
        Step('build', ['cmake', '--build', '--preset', preset, '--parallel', jobs], superbuild, env),
        Step('test', ['ctest', '--preset', preset, '-E', '^host-integration$'], superbuild, env),
        Step('integration (verbose)', ['ctest', '--preset', preset, '-V', '-R', '^host-integration$',
                                      '--no-tests=error'], superbuild, env),
        Step('installed packages', [python(), 'host/test/install_smoke.py', '--build',
                                    f'host/superbuild/build/{preset}'], ROOT, env),
    ]


def plan_host_win(jobs):
    missing = [str(d) for d in QT_DIRS if not d.is_dir()]
    if missing:
        raise Unavailable('Qt tools not found: ' + ', '.join(missing))
    env = dict(os.environ)
    env['PATH'] = ';'.join([*map(str, QT_DIRS), env['PATH']])
    return plan_host('windows-mingw', 'windows-mingw', jobs, env)


def worktree_key():
    return f'{ROOT.name}-{hashlib.sha256(str(ROOT).encode()).hexdigest()[:8]}'


def plan_host_linux(jobs):
    if not WINDOWS:
        need('cmake', 'ninja', 'g++')
        return plan_host('linux', 'linux-no-gui', jobs)
    # From Windows the Linux build runs in WSL. Its build directory is on the
    # Linux filesystem (no rename locks) and outside /tmp, which WSL keeps in
    # RAM and loses whenever the idle VM stops.
    distro = os.environ.get('DRIVELINK_WSL_DISTRO')
    wsl = ['wsl.exe', *(['-d', distro] if distro else [])]
    env = dict(os.environ, WSL_UTF8='1')
    need('wsl.exe')
    probe = subprocess.run([*wsl, '--exec', 'sh', '-c',
                            'command -v cmake ninja g++ python3 >/dev/null && printf %s $HOME'],
                           env=env, capture_output=True, text=True, errors='replace')
    home = probe.stdout.strip()
    if probe.returncode != 0 or not home.startswith('/'):
        raise Unavailable('WSL needs cmake, ninja, g++ and python3 '
                          '(apt install build-essential cmake ninja-build python3)')
    build = f'{home}/.cache/drivelink/{worktree_key()}/host-linux'

    def inside(title, cwd, *command):
        return Step(title, [*wsl, '--cd', str(cwd), '--exec', *command], env=env)
    superbuild = ROOT / 'host/superbuild'
    return [
        inside('configure', superbuild, 'cmake', '--preset', 'linux-no-gui', '-B', build),
        inside('build', superbuild, 'cmake', '--build', build, '--parallel', jobs),
        inside('test', superbuild, 'ctest', '--test-dir', build, '--output-on-failure',
               '-E', '^host-integration$'),
        inside('integration (verbose)', superbuild, 'ctest', '--test-dir', build, '-V',
               '-R', '^host-integration$', '--no-tests=error'),
        inside('installed packages', ROOT, 'python3', 'host/test/install_smoke.py', '--build', build),
    ]


NOLOG = ['-B', 'build_variant', '-D', 'SDKCONFIG=build_variant/sdkconfig',
         '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.nolog']

CHECKS = [
    Check('gen', 'generated files, generator tests, Python frame reference', plan_gen, reads_docs=True),
    Check('native', 'native unit tests (firmware/test/native)', plan_native),
    Check('host-win', 'host build and tests, Qt MinGW', plan_host_win, windows_only=True),
    Check('host-linux', 'host build and tests, Linux (through WSL on Windows)', plan_host_linux),
    # Each IDF app is built from inside its directory, so every relative path
    # (build dir, sdkconfig, defaults files) resolves against the app.
    Check('fw', 'firmware build, zero warnings', idf_build('firmware'),
          build_dir=ROOT / 'firmware/build'),
    Check('fw-nolog', 'firmware build with console logs off, zero warnings',
          idf_build('firmware', *NOLOG), build_dir=ROOT / 'firmware/build_variant'),
    Check('fw-hil-app', 'HIL test app build, zero warnings', idf_build('firmware/test/uart_comm'),
          build_dir=ROOT / 'firmware/test/uart_comm/build'),
]


def select(mode, only, skip, windows=WINDOWS):
    """The names to run, in order, and the names left out of a full run."""
    here = [c.name for c in CHECKS if windows or not c.windows_only]
    unknown = [name for name in [*(only or []), *(skip or [])] if name not in here]
    if unknown:
        raise SystemExit(f"unknown check on this OS: {', '.join(unknown)} (have: {', '.join(here)})")
    quick = ['gen', 'native', 'host-win' if windows else 'host-linux']
    wanted = only or (quick if mode == 'quick' else here)
    chosen = [name for name in here if name in wanted and name not in (skip or [])]
    return chosen, [name for name in here if name not in chosen]


# --- Unchanged-code detection ----------------------------------------------

def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, check=True, capture_output=True).stdout


def fingerprint(check):
    """A hash of the files the check covers, or None when it can't be known.

    It hashes the files as they are on disk (tracked or new, not ignored), so
    staging or committing them does not change it.
    """
    if CI:
        return None
    spec = ['.', *(f':(exclude){path}' for path in NOT_CODE), *([] if check.reads_docs else DOCS)]
    try:
        listing = git('ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', *spec)
    except (OSError, subprocess.CalledProcessError):
        return None
    digest = hashlib.sha256()
    for path in sorted(set(listing.decode('utf-8', 'surrogateescape').split('\0')) - {''}):
        file = ROOT / path
        digest.update(path.encode('utf-8', 'surrogateescape') + b'\0')
        digest.update(hashlib.sha256(file.read_bytes()).digest() if file.is_file() else b'gone')
    return digest.hexdigest()


def revision():
    try:
        dirty = ' + uncommitted changes' if git('status', '--porcelain') else ''
        return git('rev-parse', '--short', 'HEAD').decode().strip() + dirty
    except (OSError, subprocess.CalledProcessError):
        return 'unknown revision'


# --- Running ---------------------------------------------------------------

def warning_lines(output):
    return [line for line in output.splitlines() if 'warning:' in line]


def execute(program, step, env, log, verbose):
    lines = []
    with subprocess.Popen([program, *step.argv[1:]], cwd=step.cwd, env=env,
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as process:
        for raw in process.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            lines.append(line)
            log.write(line + '\n')
            if verbose:
                print(line, flush=True)
    log.flush()
    return process.returncode, lines


def run_step(name, step, log, verbose):
    """Run one step. Returns True if it passed only after a compiler-crash retry."""
    env = step.env or os.environ
    program = shutil.which(step.argv[0], path=env.get('PATH'))
    if not program:
        raise Failed(f'{step.title}: {step.argv[0]} not found')
    header = f"$ {' '.join(map(str, step.argv))}   (in {step.cwd})"
    log.write(header + '\n')
    if GROUPS:
        print(f'::group::{name}: {step.title}', flush=True)
    if verbose:
        print(header, flush=True)
    code, lines = execute(program, step, env, log, verbose)
    # The Windows xtensa GCC now and then segfaults on an IDF source in a full
    # build (firmware/CLAUDE.md, "Build traps"). The build tool resumes where
    # it stopped, so run the step once more. A crash that is not random fails
    # again, and the retry is named in the result.
    crashed = code != 0 and any('internal compiler error' in line for line in lines)
    if crashed:
        log.write('Compiler crash: running this step once more\n')
        code, again = execute(program, step, env, log, verbose)
        lines += again
    if GROUPS:
        print('::endgroup::', flush=True)
    if code != 0:
        raise Failed(f'{step.title} failed (exit {code})', '\n'.join(lines[-25:]))
    warnings = warning_lines('\n'.join(lines)) if step.warning_free else []
    if warnings:
        raise Failed(f'{step.title} has compiler warnings; the build must stay warning-free',
                     '\n'.join(warnings[:25]), warned=True)
    return crashed


def run_check(check, steps, previous, verbose):
    """Run the steps. Returns True if a step needed its compiler-crash retry."""
    with open(OUT / f'{check.name}.log', 'w', encoding='utf-8') as log:
        if previous.get('warned') and check.build_dir and check.build_dir.is_dir():
            # Ninja would not recompile the file that warned, and the warning
            # would be gone from the next log. Build it all again instead.
            log.write(f'The last run had warnings: removing {check.build_dir}\n')
            shutil.rmtree(check.build_dir)
        return any([run_step(check.name, step, log, verbose) for step in steps])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('mode', nargs='?', choices=['quick', 'full'], default='quick')
    parser.add_argument('--only', nargs='+', metavar='CHECK', help='run just these checks')
    parser.add_argument('--skip', nargs='+', metavar='CHECK', help='leave these checks out')
    parser.add_argument('--force', action='store_true', help='also run checks whose code is unchanged')
    parser.add_argument('--fail-fast', action='store_true', help='stop at the first failure')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4,
                        help='compile jobs for the CMake builds (default: all CPUs)')
    parser.add_argument('-v', '--verbose', action='store_true', help='show the command output')
    parser.add_argument('--list', action='store_true', help='list the checks and exit')
    args = parser.parse_args(argv)
    sys.stdout.reconfigure(errors='replace')

    if args.list:
        for check in CHECKS:
            print(f"{check.name:<11} {check.about}{' (Windows only)' if check.windows_only else ''}")
        return 0
    chosen, left_out = select(args.mode, args.only, args.skip)
    verbose = args.verbose or GROUPS
    OUT.mkdir(parents=True, exist_ok=True)
    state_file = OUT / 'state.json'
    try:
        state = json.loads(state_file.read_text())
    except (OSError, ValueError):
        state = {}

    # Decide every check's plan first, so missing tools are all reported at
    # once and not after ten minutes of other checks.
    work = []
    for check in (c for c in CHECKS if c.name in chosen):
        covered = fingerprint(check)
        previous = state.get(check.name, {})
        if not args.force and covered and previous == {'result': 'pass', 'fingerprint': covered}:
            work.append((check, None, covered, 'pass (unchanged)'))
            continue
        try:
            work.append((check, check.plan(str(args.jobs)), covered, None))
        except Unavailable as missing:
            work.append((check, None, covered, f'UNAVAILABLE: {missing}'))

    title = ' '.join(args.only) if args.only else args.mode
    summary = [f"DriveLink validation ({title}) at {revision()}, {time.strftime('%Y-%m-%d %H:%M')}"]
    print(summary[0], flush=True)
    for check, _, _, result in work:
        if result and result.startswith('UNAVAILABLE'):
            print(f"  {check.name} cannot run here ({result.partition(': ')[2]})", flush=True)
    bad = 0
    for check, steps, covered, result in work:
        detail, seconds = '', ''
        if result is None and bad and args.fail_fast:
            result = 'not run (--fail-fast)'
        elif result is None:
            print(f'  {check.name:<11} running...', flush=True)
            started = time.monotonic()
            entry = {'result': 'fail'}
            try:
                retried = run_check(check, steps, state.get(check.name, {}), verbose)
                result = 'pass (after one retry of a compiler crash)' if retried else 'pass'
                entry = {'result': 'pass', 'fingerprint': covered}
            except Failed as failure:
                result, detail = f'FAIL: {failure}', failure.detail
                entry['warned'] = failure.warned
                if GROUPS:
                    print(f'::error::{check.name}: {failure}', flush=True)
            seconds = f'{time.monotonic() - started:.0f} s'
            state[check.name] = entry
            state_file.write_text(json.dumps(state, indent=1))
        bad += not result.startswith('pass')
        line = f'  {check.name:<11} {result}' + (f'   [{seconds}]' if seconds else '')
        if result.startswith('FAIL'):
            line += f'\n              log: build/validate/{check.name}.log'
        summary.append(line)
        print(line, flush=True)
        if detail:
            print('\n'.join('      | ' + text for text in detail.splitlines()), flush=True)

    if left_out:
        summary.append(f"  not selected: {', '.join(left_out)}")
    summary.append('  hardware: not run (this script never flashes a board or opens a port)')
    verdict = f'FAIL ({bad} of {len(work)} checks)' if bad else 'PASS'
    if not bad and left_out:
        verdict += f' for {len(work)} of {len(work) + len(left_out)} checks; not the full gate'
    summary.append(f'Result: {verdict}')
    print('\n'.join(summary[-3:] if left_out else summary[-2:]), flush=True)
    (OUT / 'summary.txt').write_text('\n'.join(summary) + '\n', encoding='utf-8')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
