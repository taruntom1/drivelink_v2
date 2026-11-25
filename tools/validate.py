#!/usr/bin/env python3
"""Run DriveLink's validation checks: one list, used locally and by CI.

    python tools/validate.py quick          # generator, native tests, this OS's host tests (+ agent on Linux)
    python tools/validate.py full           # every check below (docs/development.md says when)
    python tools/validate.py --only fw-nolog
    python tools/validate.py --list

Checks run one at a time, so a build never competes with another build or
with a timing-sensitive test. Each check sets up its own toolchain. Logs and
the summary go to build/validate/. A check that passed is not run again while
the code it covers is unchanged (--force runs it anyway).

No check flashes a board or opens a serial port. Hardware tests stay separate
(firmware/CLAUDE.md, "Hardware-in-the-loop and smoke tests").

Standard library only. The guide is in docs/development.md ("Validation").
"""
import argparse
import dataclasses
import datetime
import functools
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'validate'  # ignored by .gitignore (build/)
WINDOWS = os.name == 'nt'
CI = bool(os.environ.get('CI') or os.environ.get('GITHUB_ACTIONS'))
GROUPS = bool(os.environ.get('GITHUB_ACTIONS'))

# This machine's toolchains, documented in docs/development.md and host/README.md.
# The windows-mingw preset and install_smoke.py name the same Qt compiler.
QT_TOOLS = Path('C:/Qt/Tools')
QT_DIRS = [QT_TOOLS / 'mingw1310_64/bin', QT_TOOLS / 'CMake_64/bin', QT_TOOLS / 'Ninja']
UCRT_BIN = Path('C:/msys64/ucrt64/bin')  # MSYS2 UCRT64 GCC 16.2; native tests only
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
    inputs: tuple = None         # git pathspecs this check reads (None = whole repo)


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
        env.setdefault('CCACHE_BASEDIR', str(ROOT))
        env.setdefault('CCACHE_NOHASHDIR', '1')
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
    exported.setdefault('CCACHE_BASEDIR', str(ROOT))
    exported.setdefault('CCACHE_NOHASHDIR', '1')
    return exported


def idf_build(cwd, *options):
    def plan(jobs):
        env = idf_env()
        idf_py = str(Path(env['IDF_PATH']) / 'tools' / 'idf.py')
        # idf.py has no job option for Ninja; the build runs alone instead.
        return [Step('idf.py build', ['python', idf_py, *options, 'build'],
                     cwd=ROOT / cwd, env=env, warning_free=True)]
    return plan


def fw_version_matches():
    """True when firmware/build already configured this checkout's exact version."""
    if CI:
        return False
    desc = ROOT / 'firmware/build/project_description.json'
    if not desc.is_file():
        return False
    try:
        built = json.loads(desc.read_text(encoding='utf-8')).get('project_version')
    except (OSError, ValueError):
        return False
    sys.path.insert(0, str(ROOT / 'tools'))
    import version
    return built == version.full()


def plan_fw_package(jobs):
    """Reconfigure when the cached version differs (or in CI), build, package."""
    options = () if fw_version_matches() else ('reconfigure',)
    build = idf_build('firmware', *options)(jobs)
    return [*build, Step('package', ['python', 'tools/package_fw.py'], env=build[0].env)]


def plan_gen(jobs):
    need_yaml()
    return [
        Step('generated files are current', [python(), 'tools/gen/gen.py', '--check']),
        Step('versions match VERSION', [python(), 'tools/version.py', '--check']),
        Step('generator tests', [python(), '-m', 'unittest', 'discover', '-s', 'tools/gen/tests']),
        Step('validate.py tests', [python(), '-m', 'unittest', 'discover', '-s', 'tools/tests']),
        Step('Python frame reference', [python(), 'firmware/test/uart_comm/protocol.py']),
        Step('GUI Theme is current', [python(), 'host/gui/theme/gen_theme.py', '--check']),
        Step('GUI Theme generator tests', [python(), '-m', 'unittest', 'discover',
                                           '-s', 'host/gui/theme', '-p', 'test_*.py']),
        Step('third-party notices are current', [python(), 'host/gui/notices/gen_notices.py', '--check']),
        Step('firmware notices are current', [python(), 'host/gui/notices/gen_notices.py', '--check',
                                              '--manifest', 'firmware/notices/notices.json',
                                              '--output', 'firmware/notices/THIRD_PARTY_NOTICES.txt']),
        Step('CLI and agent package notices are current', [python(), 'host/gui/notices/gen_notices.py', '--check',
                                                           '--manifest', 'host/notices/notices.json',
                                                           '--output', 'host/notices/THIRD_PARTY_NOTICES.txt']),
        Step('notices generator tests', [python(), '-m', 'unittest', 'discover',
                                         '-s', 'host/gui/notices', '-p', 'test_*.py']),
    ]


def plan_native(jobs):
    need_yaml()
    build = 'firmware/test/native/build'
    env = None
    cmake, ctest = 'cmake', 'ctest'
    options = []
    if WINDOWS:
        # Name every tool so Strawberry (or Qt's MinGW) cannot win via PATH.
        cmake = (QT_DIRS[1] / 'cmake.exe').as_posix()
        ctest = (QT_DIRS[1] / 'ctest.exe').as_posix()
        ninja = (QT_DIRS[2] / 'ninja.exe').as_posix()
        cc, cxx = [(UCRT_BIN / name).as_posix() for name in ('gcc.exe', 'g++.exe')]
        missing = [tool for tool in (cmake, ctest, ninja, cc, cxx) if not Path(tool).is_file()]
        if missing:
            raise Unavailable('native tests need MSYS2 UCRT64 GCC 16.2 and Qt CMake/Ninja; '
                              'missing: ' + ', '.join(missing))
        env = dict(os.environ)
        env['PATH'] = ';'.join([str(UCRT_BIN), *map(str, QT_DIRS[1:]), env['PATH']])
        build += '/ucrt64'  # never reuse a Strawberry CMake cache or object
        options = [f'-DCMAKE_C_COMPILER={cc}', f'-DCMAKE_CXX_COMPILER={cxx}',
                   f'-DCMAKE_MAKE_PROGRAM={ninja}']
    else:
        need('cmake', 'ctest', 'ninja', 'gcc', 'g++')
    return [
        Step('configure', [cmake, '-S', 'firmware/test/native', '-B', build, '-G', 'Ninja',
                           f'-DPython3_EXECUTABLE={python()}', *options], env=env),
        Step('build', [cmake, '--build', build, '--parallel', jobs], env=env),
        Step('test', [ctest, '--test-dir', build, '--output-on-failure'], env=env),
    ]


def ctest_jobs(jobs):
    """Cap CTest parallelism so offscreen Qt and vdev processes don't oversubscribe."""
    return str(max(1, min(4, int(jobs))))


def deps_cache_key():
    """Short hash of pinned FetchContent specs so shared caches rotate on updates."""
    digest = hashlib.sha256()
    for rel in ('host/cmake/Dependencies.cmake', 'host/cmake/Json.cmake'):
        file = ROOT / rel
        digest.update(file.read_bytes() if file.is_file() else b'missing')
    return digest.hexdigest()[:12]


def wsl_cache_env(env, home):
    """Pass shared Linux ccache and FetchContent cache paths into WSL."""
    cache_root = f'{home}/.cache/drivelink'
    merged = dict(env or os.environ)
    merged.setdefault('CCACHE_DIR', f'{cache_root}/ccache')
    merged.setdefault('CCACHE_NOHASHDIR', '1')
    merged.setdefault('DRIVELINK_DEPS_CACHE', f'{cache_root}/deps-{deps_cache_key()}')
    names = [item for item in merged.get('WSLENV', '').split(':') if item]
    for name in ('CCACHE_DIR', 'CCACHE_NOHASHDIR', 'DRIVELINK_DEPS_CACHE'):
        if name not in names:
            names.append(name)
    merged['WSLENV'] = ':'.join(names)
    return merged


def plan_host(preset, configure_preset, jobs, env=None):
    superbuild = ROOT / 'host/superbuild'
    return [
        Step('configure', ['cmake', '--preset', configure_preset], superbuild, env),
        Step('build', ['cmake', '--build', '--preset', preset, '--parallel', jobs], superbuild, env),
        Step('test', ['ctest', '--preset', preset, '--parallel', ctest_jobs(jobs), '-E', '^host-integration$'],
             superbuild, env),
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
    # Fail explicitly if the selected directory contains another compiler;
    # neither runner PATH defaults nor another MinGW kit enforce our floor.
    compiler = QT_DIRS[0] / 'g++.exe'
    try:
        version = subprocess.run([str(compiler), '-dumpfullversion'], env=env,
                                 capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise Unavailable(f'cannot query Qt MinGW compiler {compiler}: {error}') from error
    if version != '13.1.0':
        raise Unavailable(f'host-win requires Qt MinGW GCC 13.1.0, found {version!r}')
    return plan_host('windows-mingw', 'windows-mingw', jobs, env)


ISCC_DIRS = [Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/Inno Setup 6',
             Path('C:/Program Files (x86)/Inno Setup 6')]


def plan_installer(jobs):
    """The Release staging directory, its self-contained smoke and the installer."""
    plan_host_win(jobs)  # the same Qt MinGW 13.1 prerequisites
    iscc = shutil.which('ISCC') or next((str(d / 'ISCC.exe') for d in ISCC_DIRS if (d / 'ISCC.exe').is_file()), None)
    if not iscc:
        raise Unavailable('Inno Setup 6 not found; run: winget install --id JRSoftware.InnoSetup -e')
    env = dict(os.environ)
    env['PATH'] = ';'.join([*map(str, QT_DIRS), env['PATH']])
    return [Step('package and installer', [python(), 'tools/package_win.py', '--jobs', jobs, '--iscc', iscc],
                 env=env)]


def worktree_key():
    return f'{ROOT.name}-{hashlib.sha256(str(ROOT).encode()).hexdigest()[:8]}'


def linux_packages_hint(profile):
    what = {'agent': 'compilers, CMake, Ninja, Python 3', 'base': 'compilers, CMake, Ninja, Python 3',
            'ros': 'ROS 2 Lyrical or Jazzy with ros2_control, colcon'}.get(profile, 'compilers, CMake, Ninja, Qt 6.10')
    return f'Linux {profile} packages missing ({what}); run: sudo sh tools/setup_linux.sh {profile}'


def linux_probe(profile='host'):
    """Native Linux: None. Windows: (wsl command, env, $HOME) for the WSL distro.

    `profile` is the tools/setup_linux.sh profile the check needs.
    """
    hint = linux_packages_hint(profile)
    if not WINDOWS:
        need('cmake', 'ninja', 'g++')
        probe = subprocess.run(['sh', str(ROOT / 'tools/setup_linux.sh'), '--check', profile],
                               capture_output=True, text=True, errors='replace')
        if probe.returncode != 0:
            raise Unavailable(hint)
        return None
    distro = os.environ.get('DRIVELINK_WSL_DISTRO')
    wsl = ['wsl.exe', *(['-d', distro] if distro else [])]
    env = dict(os.environ, WSL_UTF8='1')
    need('wsl.exe')
    probe = subprocess.run([*wsl, '--cd', str(ROOT), '--exec', 'sh', '-c',
                            f'sh tools/setup_linux.sh --check {profile} >/dev/null && printf %s $HOME'],
                           env=env, capture_output=True, text=True, errors='replace')
    home = probe.stdout.strip()
    if probe.returncode != 0 or not home.startswith('/'):
        raise Unavailable(f'WSL: {hint}')
    return wsl, env, home


def plan_host_linux(jobs):
    probe = linux_probe()
    if probe is None:
        return plan_host('linux', 'linux', jobs)
    # From Windows the Linux build runs in WSL. Its build directory is on the
    # Linux filesystem (no rename locks) and outside /tmp, which WSL keeps in
    # RAM and loses whenever the idle VM stops.
    wsl, env, home = probe
    env = wsl_cache_env(env, home)
    build = f'{home}/.cache/drivelink/{worktree_key()}/host-linux'
    deps = env['DRIVELINK_DEPS_CACHE']

    def inside(title, cwd, *command):
        return Step(title, [*wsl, '--cd', str(cwd), '--exec', *command], env=env)
    superbuild = ROOT / 'host/superbuild'
    return [
        inside('configure', superbuild, 'cmake', '--preset', 'linux', '-B', build,
               f'-DFETCHCONTENT_BASE_DIR={deps}'),
        inside('build', superbuild, 'cmake', '--build', build, '--parallel', jobs),
        inside('test', superbuild, 'ctest', '--test-dir', build, '--output-on-failure',
               '--parallel', ctest_jobs(jobs), '-E', '^host-integration$', '-LE', 'agent'),
        inside('integration (verbose)', superbuild, 'ctest', '--test-dir', build, '-V',
               '-R', '^host-integration$', '--no-tests=error'),
        inside('installed packages', ROOT, 'python3', 'host/test/install_smoke.py', '--build', build),
    ]


def plan_agent(jobs):
    """drivelink-agent: the GUI-less superbuild, then the tests labelled `agent`."""
    probe = linux_probe('agent')
    superbuild = ROOT / 'host/superbuild'
    test = ['ctest', '--output-on-failure', '-L', 'agent', '--no-tests=error']
    if probe is None:
        build = superbuild / 'build/linux-no-gui'
        return [
            Step('configure', ['cmake', '--preset', 'linux-no-gui'], superbuild),
            Step('build', ['cmake', '--build', str(build), '--parallel', jobs], superbuild),
            Step('test', [*test[:1], '--test-dir', str(build), *test[1:]], superbuild),
        ]
    # As host-linux: the WSL build directory is on the Linux filesystem.
    wsl, env, home = probe
    env = wsl_cache_env(env, home)
    build = f'{home}/.cache/drivelink/{worktree_key()}/agent'
    deps = env['DRIVELINK_DEPS_CACHE']

    def inside(title, *command):
        return Step(title, [*wsl, '--cd', str(superbuild), '--exec', *command], env=env)
    return [
        inside('configure', 'cmake', '--preset', 'linux-no-gui', '-B', build,
               f'-DFETCHCONTENT_BASE_DIR={deps}'),
        inside('build', 'cmake', '--build', build, '--parallel', jobs),
        inside('test', *test[:1], '--test-dir', build, *test[1:]),
    ]


def plan_deb(jobs):
    """The Linux release build, its staged smoke and the .deb packages (build/deb/):
    drivelink-cli and drivelink-agent, and the GUI's drivelink where Ubuntu's Qt
    meets its floor (26.04, the WSL distro; not 24.04)."""
    profile = 'host'
    if not WINDOWS:
        sys.path.insert(0, str(ROOT / 'tools'))
        import package_linux
        profile = 'host' if package_linux.gui_supported(*package_linux.os_release()) else 'agent'
    probe = linux_probe(profile)
    title = 'package and .deb'
    if probe is None:
        return [Step(title, ['python3', 'tools/package_linux.py', '--jobs', jobs])]
    # WSL's git cannot read a Windows worktree: pass this checkout's version.
    # The stage needs a Linux filesystem (dpkg-deb checks file modes).
    wsl, env, home = probe
    env = wsl_cache_env(env, home)
    sys.path.insert(0, str(ROOT / 'tools'))
    import version
    env = dict(env, DRIVELINK_VERSION_FULL=version.full(),
               WSLENV=':'.join(filter(None, [env.get('WSLENV'), 'DRIVELINK_VERSION_FULL'])))
    build = f'{home}/.cache/drivelink/{worktree_key()}/linux-release'
    return [Step(title, [*wsl, '--cd', str(ROOT), '--exec', 'python3', 'tools/package_linux.py',
                         '--jobs', jobs, '--build-dir', build], env=env)]


def plan_apt_repo(jobs):
    """The H3d apt repository built from the `deb` check's packages (build/deb, or
    DRIVELINK_APT_DEBS: CI passes every release's) with a throwaway key, then
    checked by apt itself: signature, indices, resolution, and that another key,
    no signature or a changed index is refused."""
    probe = linux_probe('base')
    debs = os.environ.get('DRIVELINK_APT_DEBS') or 'build/deb'
    if probe is None:
        need('gpg', 'dpkg-deb', 'apt-get')
        out = (ROOT / 'build/apt').as_posix()
        wrap, env = [], None
    else:
        # The repository is written on the Linux filesystem (pool copies, file modes).
        wsl, env, home = probe
        out = f'{home}/.cache/drivelink/{worktree_key()}/apt-repo'
        wrap = [*wsl, '--cd', str(ROOT), '--exec']
    key = f'{out}-key.asc'
    tool = [*wrap, 'python3', 'tools/apt_repo.py']
    return [
        Step('build (throwaway key)', [*tool, 'build', '--debs', debs, '--out', out, '--clean',
                                       '--throwaway-key', '--public-key-out', key,
                                       *(['--complete'] if CI else [])], env=env),
        Step('verify with apt', [*tool, 'verify', '--repo', out, '--key', key], env=env),
    ]


ROS_PHASES = [('superbuild (agent, vdev, cli)', 'superbuild'), ('colcon build', 'build'), ('colcon test', 'test')]


def version_env(env):
    """env plus DRIVELINK_VERSION_FULL (passed through WSLENV): a Linux build of a
    Windows worktree cannot ask git for the version."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import version
    env = dict(env or os.environ, DRIVELINK_VERSION_FULL=version.full())
    env['WSLENV'] = ':'.join(filter(None, [env.get('WSLENV'), 'DRIVELINK_VERSION_FULL', 'DRIVELINK_JOBS']))
    return env


def plan_ros(jobs):
    """drivelink_hardware (H4a): the agent, vdev and CLI from the superbuild, then
    colcon build and test of protocol/, host/libdrivelink and host/ros against
    the installed ROS 2 (Lyrical in WSL; the ROS image's distribution in CI)."""
    probe = linux_probe('ros')
    if probe is None:
        build, wrap, env = (ROOT / 'build/ros').as_posix(), [], None
    else:
        wsl, env, home = probe
        env = wsl_cache_env(env, home)
        build = f'{home}/.cache/drivelink/{worktree_key()}/ros'
        wrap = [*wsl, '--cd', str(ROOT), '--exec']
    env = dict(version_env(env), DRIVELINK_JOBS=jobs)
    return [Step(title, [*wrap, 'sh', 'tools/ros_check.sh', phase, build], env=env, warning_free=True)
            for title, phase in ROS_PHASES]


def need_docker():
    need('docker')
    alive = subprocess.run(['docker', 'info', '--format', '{{.ServerVersion}}'],
                           capture_output=True, text=True, errors='replace')
    if alive.returncode != 0:
        raise Unavailable('Docker is not running: start Docker Desktop or the Docker daemon')


def plan_ros_jazzy(jobs):
    """Run the source checks in Docker with a read-only checkout and a build volume."""
    need_docker()
    image = 'drivelink-ros:jazzy'
    env = dict(version_env(None), DRIVELINK_JOBS=jobs)
    deps = f'/cache/deps-{deps_cache_key()}'
    run = ['docker', 'run', '--rm', '-v', f'{ROOT}:/src:ro', '-w', '/src',
           '-v', 'drivelink-ros-jazzy:/cache', '-e', 'DRIVELINK_VERSION_FULL', '-e', 'DRIVELINK_JOBS',
           '-e', 'CCACHE_DIR=/cache/ccache', '-e', 'CCACHE_BASEDIR=/src', '-e', 'CCACHE_NOHASHDIR=1',
           '-e', f'DRIVELINK_DEPS_CACHE={deps}', image]
    build = f'/cache/{worktree_key()}'
    return [Step('image (ros:jazzy + setup_linux.sh ros)',
                 ['docker', 'build', '-q', '-t', image, '--build-arg', 'DISTRO=jazzy',
                  '-f', str(ROOT / 'tools/ros.Dockerfile'), str(ROOT / 'tools')], env=env),
            *(Step(title, [*run, 'sh', 'tools/ros_check.sh', phase, build], env=env, warning_free=True)
              for title, phase in ROS_PHASES)]


def ros_deb_docker():
    need_docker()
    out = ROOT / 'build/ros-debs'
    out.mkdir(parents=True, exist_ok=True)
    return ['docker', 'run', '--rm', '-v', f'{ROOT}:/src:ro', '-w', '/src', '-v', f'{out}:/out']


def plan_ros_deb(jobs):
    package = ['python3', 'tools/package_ros.py', '--jobs', jobs]
    if CI:
        return [Step('Bloom build', package, warning_free=True)]
    run = [*ros_deb_docker(), '-v', 'drivelink-ros-deb-cache:/cache',
           '-e', 'CCACHE_DIR=/cache/ccache', '-e', 'DRIVELINK_DEPS_CACHE=/cache/deps']
    env = version_env(None)
    steps = []
    for distro in ('jazzy', 'lyrical'):
        image = f'drivelink-ros-deb:{distro}'
        steps.extend((
            Step(f'{distro} package build image',
                 ['docker', 'build', '-q', '-t', image, '--build-arg', f'DISTRO={distro}',
                  '--build-arg', 'PROFILE=ros-deb', '-f', str(ROOT / 'tools/ros.Dockerfile'),
                  str(ROOT / 'tools')]),
            Step(f'{distro} Bloom build', [*run, '-e', 'DRIVELINK_VERSION_FULL', image,
                                          *package, '--out', f'/out/{distro}'],
                 env=env, warning_free=True),
        ))
    return steps


def plan_ros_deb_install(jobs):
    smoke = ['python3', 'host/test/apt_repo_smoke.py', '--ros']
    if CI:
        return [Step('signed apt install and launch', [*smoke, '--repo', 'apt-repo/apt',
                                                      '--key', 'apt-repo/apt-key.asc'])]
    run = ros_deb_docker()
    smoke += ['--repo', '/out/apt', '--key', '/out/apt-key.asc']
    image = 'drivelink-ros-deb:jazzy'
    deps = f'/cache/deps-{deps_cache_key()}'
    return [
        Step('Ubuntu 24.04 agent and CLI packages',
             [*run, '-e', 'DRIVELINK_VERSION_FULL', '-v', 'drivelink-ros-jazzy:/cache',
              '-e', 'CCACHE_DIR=/cache/ccache', '-e', 'CCACHE_BASEDIR=/src', '-e', 'CCACHE_NOHASHDIR=1',
              '-e', f'DRIVELINK_DEPS_CACHE={deps}', image,
              'python3', 'tools/package_linux.py', '--jobs', jobs, '--out', '/out/linux-jazzy',
              '--build-dir', f'/cache/{worktree_key()}/linux-release'], env=version_env(None)),
        Step('sign the two suites (throwaway key)',
             [*run, image, 'python3', 'tools/apt_repo.py', 'build', '--debs', 'build/deb',
              '--debs', '/out/linux-jazzy', '--debs', '/out/jazzy', '--debs', '/out/lyrical',
              '--out', '/out/apt', '--clean', '--throwaway-key', '--public-key-out', '/out/apt-key.asc']),
        *(Step(f'{distro} signed apt install and launch',
               [*run, '-v', f'drivelink-apt-cache-{distro}:/var/cache/apt/archives', f'ros:{distro}',
                'sh', '-c', 'rm -f /etc/apt/apt.conf.d/docker-clean && exec "$@"', '--', *smoke])
          for distro in ('jazzy', 'lyrical')),
    ]


def plan_guide(jobs):
    executable = ROOT / ('build/guide-venv/Scripts/python.exe' if WINDOWS
                         else 'build/guide-venv/bin/python')
    interpreter = str(executable) if executable.is_file() else python()
    return [
        Step('guide build tests', [interpreter, '-m', 'unittest', 'discover',
                                   '-s', 'guide/tests']),
        Step('build pages and search', [interpreter, 'tools/guide.py', 'build'], warning_free=True),
        Step('check the built guide', [interpreter, 'tools/guide.py', 'check']),
    ]


NOLOG = ['-B', 'build_variant', '-D', 'SDKCONFIG=build_variant/sdkconfig',
         '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.nolog']

# Per-check input pathspecs so unchanged subsystems reuse their last pass.
HOST_COMMON_INPUTS = (
    'host/superbuild', 'host/cmake', 'host/libdrivelink', 'host/vdev', 'host/cli', 'host/test',
    'protocol', 'firmware/components', 'firmware/test/uart_comm/protocol.py',
    'firmware/test/native/third_party/doctest', 'cmake', 'VERSION', 'LICENSE', 'tools/validate.py',
)
FW_APP_INPUTS = (
    'firmware', ':(exclude)firmware/test', ':(exclude)firmware/notices',
    'protocol', 'cmake/DriveLinkVersion.cmake', 'VERSION', 'tools/validate.py',
)

CHECKS = [
    Check('gen', 'generated files and Theme, generator tests, Python frame reference', plan_gen, reads_docs=True),
    Check('guide', 'user guide build, search, links and unit tests', plan_guide, reads_docs=True,
          inputs=('guide', 'protocol/defs', 'tools/gen', 'tools/guide.py', 'tools/version.py',
                  'tools/validate.py', 'host/gui/theme', 'packaging/apt', 'VERSION', 'LICENSE')),
    Check('native', 'native unit tests (firmware/test/native)', plan_native,
          inputs=('firmware/test/native', 'firmware/components', 'protocol', 'tools/gen', 'tools/validate.py')),
    Check('host-win', 'host build and tests, Qt MinGW', plan_host_win, windows_only=True,
          inputs=('host', ':(exclude)host/ros', ':(exclude)host/agent', 'protocol', 'firmware/components',
                  'firmware/test/uart_comm/protocol.py', 'firmware/test/native/third_party/doctest',
                  'cmake', 'VERSION', 'LICENSE', 'tools/validate.py')),
    Check('installer', 'Release staging, self-contained smoke and Inno Setup installer', plan_installer,
          windows_only=True,
          inputs=('host', ':(exclude)host/ros', ':(exclude)host/agent', ':(exclude)host/test',
                  'protocol', 'firmware/components', 'cmake', 'VERSION', 'LICENSE',
                  'tools/package_win.py', 'tools/package_common.py', 'tools/version.py', 'tools/validate.py')),
    Check('host-linux', 'host build and tests, Linux (through WSL on Windows)', plan_host_linux,
          inputs=('host', ':(exclude)host/ros', 'protocol', 'firmware/components',
                  'firmware/test/uart_comm/protocol.py', 'firmware/test/native/third_party/doctest',
                  'cmake', 'VERSION', 'LICENSE', 'tools/setup_linux.sh', 'tools/validate.py')),
    Check('agent', 'agent build, unit and integration tests (through WSL on Windows)', plan_agent,
          inputs=(*HOST_COMMON_INPUTS, 'host/agent', 'tools/setup_linux.sh')),
    Check('deb', 'Linux release, staged smoke and the .deb packages (through WSL on Windows)', plan_deb,
          inputs=('host', ':(exclude)host/ros', ':(exclude)host/test', 'protocol', 'firmware/components',
                  'cmake', 'VERSION', 'LICENSE', 'tools/package_linux.py', 'tools/package_common.py',
                  'tools/version.py', 'tools/setup_linux.sh', 'tools/validate.py')),
    Check('apt-repo', 'apt repository from the .deb packages, checked by apt (through WSL on Windows)',
          plan_apt_repo,
          inputs=('host', ':(exclude)host/ros', ':(exclude)host/test', 'protocol', 'firmware/components',
                  'cmake', 'VERSION', 'LICENSE', 'packaging/apt', 'tools/package_linux.py',
                  'tools/package_common.py', 'tools/apt_repo.py', 'tools/version.py',
                  'tools/setup_linux.sh', 'tools/validate.py')),
    Check('ros', 'drivelink_hardware: colcon build and tests on ROS 2 (Lyrical through WSL on Windows)',
          plan_ros,
          inputs=(*HOST_COMMON_INPUTS, 'host/agent', 'host/ros',
                  'tools/ros_check.sh', 'tools/setup_linux.sh', 'tools/version.py')),
    Check('ros-jazzy', 'drivelink_hardware: the ros check on ROS 2 Jazzy in Docker (ros:jazzy)', plan_ros_jazzy,
          inputs=(*HOST_COMMON_INPUTS, 'host/agent', 'host/ros',
                  'tools/ros_check.sh', 'tools/ros.Dockerfile', 'tools/setup_linux.sh', 'tools/version.py')),
    Check('ros-deb', 'Bloom Debian packages for Jazzy and Lyrical in Docker', plan_ros_deb,
          inputs=('protocol', 'host/libdrivelink', 'host/ros', 'host/cmake/Dependencies.cmake',
                  'host/notices', 'cmake/DriveLinkVersion.cmake', 'VERSION', 'LICENSE',
                  'tools/package_ros.py', 'tools/ros_packages.py', 'tools/package_linux.py',
                  'tools/package_common.py', 'tools/apt_repo.py', 'tools/version.py',
                  'tools/setup_linux.sh', 'tools/ros.Dockerfile', 'tools/validate.py')),
    Check('ros-deb-install', 'install the signed ROS packages and run the packaged launch in Docker',
          plan_ros_deb_install,
          inputs=('host', 'protocol', 'firmware/components', 'cmake', 'VERSION', 'LICENSE', 'packaging/apt',
                  'tools/package_linux.py', 'tools/package_ros.py', 'tools/ros_packages.py',
                  'tools/package_common.py', 'tools/apt_repo.py', 'tools/version.py',
                  'tools/setup_linux.sh', 'tools/ros.Dockerfile', 'tools/validate.py')),
    # Each IDF app is built from inside its directory, so every relative path
    # (build dir, sdkconfig, defaults files) resolves against the app.
    Check('fw', 'firmware build, zero warnings', idf_build('firmware'),
          build_dir=ROOT / 'firmware/build', inputs=FW_APP_INPUTS),
    Check('fw-nolog', 'firmware build with console logs off, zero warnings',
          idf_build('firmware', *NOLOG), build_dir=ROOT / 'firmware/build_variant', inputs=FW_APP_INPUTS),
    Check('fw-hil-app', 'HIL test app build, zero warnings', idf_build('firmware/test/uart_comm'),
          build_dir=ROOT / 'firmware/test/uart_comm/build',
          inputs=('firmware/test/uart_comm', 'firmware/components', 'protocol',
                  'cmake/DriveLinkVersion.cmake', 'VERSION', 'tools/validate.py')),
    Check('fw-package', 'firmware release package (merged image, parts, OTA app, notices)',
          plan_fw_package,
          inputs=('firmware', ':(exclude)firmware/test', 'protocol', 'cmake/DriveLinkVersion.cmake',
                  'VERSION', 'LICENSE', 'tools/package_fw.py', 'tools/package_common.py',
                  'tools/version.py', 'tools/validate.py')),
]


def select(mode, only, skip, windows=WINDOWS):
    """The names to run, in order, and the names left out of a full run."""
    here = [c.name for c in CHECKS if windows or not c.windows_only]
    unknown = [name for name in [*(only or []), *(skip or [])] if name not in here]
    if unknown:
        raise SystemExit(f"unknown check on this OS: {', '.join(unknown)} (have: {', '.join(here)})")
    quick = ['gen', 'native', *(['host-win'] if windows else ['host-linux', 'agent'])]
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
    roots = list(check.inputs) if check.inputs is not None else ['.']
    spec = [*roots, *(f':(exclude){path}' for path in NOT_CODE), *([] if check.reads_docs else DOCS)]
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


def source_snapshot():
    """Identify the invocation separately from a cached check's execution."""
    try:
        return {'revision': git('rev-parse', 'HEAD').decode().strip(),
                'dirty': bool(git('status', '--porcelain'))}
    except (OSError, subprocess.CalledProcessError):
        return {'revision': None, 'dirty': None}


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_report(report):
    """Publish only complete reports, preserving the last one if writing fails."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=OUT,
                                         prefix='summary-', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(report, stream, indent=2)
            stream.write('\n')
        os.replace(temporary, OUT / 'summary.json')
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


# --- Running ---------------------------------------------------------------

def warning_lines(output):
    return [line for line in output.splitlines() if 'warning:' in line]


def execute(program, step, env, log, verbose):
    lines = []
    # A timed-out/crashed test can leave a descendant holding stdout. A pipe's
    # EOF then never arrives, even after CTest exits. Tail a regular file until
    # the command exits instead. Independent handles keep the writer's offset
    # untouched; O_TEMPORARY lets Windows delete it after the last child closes.
    with tempfile.NamedTemporaryFile() as output:
        reader_fd = os.open(output.name, os.O_RDONLY | getattr(os, 'O_TEMPORARY', 0)
                            | getattr(os, 'O_BINARY', 0))
        with os.fdopen(reader_fd, 'rb') as reader, \
                subprocess.Popen([program, *step.argv[1:]], cwd=step.cwd, env=env,
                                 stdin=subprocess.DEVNULL, stdout=output,
                                 stderr=subprocess.STDOUT) as process:
            pending = b''
            while True:
                exited = process.poll() is not None
                pending += reader.read()
                chunks = pending.split(b'\n')
                pending = chunks.pop()
                if exited and pending:
                    chunks.append(pending)
                for raw in chunks:
                    line = raw.decode('utf-8', 'replace').rstrip('\r')
                    lines.append(line)
                    log.write(line + '\n')
                    if verbose:
                        print(line, flush=True)
                log.flush()
                if exited:
                    break
                time.sleep(0.05)
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
    report = {
        'schema_version': 1,
        'started_at': utc_now(),
        'finished_at': None,
        'source': source_snapshot(),
        'request': {'mode': args.mode, 'only': args.only, 'skip': args.skip,
                    'force': args.force, 'fail_fast': args.fail_fast},
        'status': None,
        'full_gate_passed': False,
        'hardware_status': 'not_run',
        'omitted_checks': left_out,
        'checks': [],
    }
    records = {}
    for check in CHECKS:
        if check.windows_only and not WINDOWS:
            continue
        record = {'name': check.name, 'status': 'not_selected',
                  'input_fingerprint': None, 'duration_seconds': None,
                  'log_path': None, 'failure_reason': None,
                  'diagnostic_excerpt': None, 'execution': None,
                  'compiler_crash_retried': None}
        records[check.name] = record
        report['checks'].append(record)
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
        if (not args.force and covered and previous.get('result') == 'pass'
                and previous.get('fingerprint') == covered):
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
        record = records[check.name]
        record['input_fingerprint'] = covered
        detail, seconds = '', ''
        if result is None and bad and args.fail_fast:
            result = 'not run (--fail-fast)'
            record['status'] = 'skipped_after_failure'
        elif result is None:
            print(f'  {check.name:<11} running...', flush=True)
            started = time.monotonic()
            execution = {'source': report['source'], 'started_at': utc_now(),
                         'finished_at': None}
            entry = {'result': 'fail'}
            try:
                retried = run_check(check, steps, state.get(check.name, {}), verbose)
                result = 'pass (after one retry of a compiler crash)' if retried else 'pass'
                entry = {'result': 'pass', 'fingerprint': covered}
                record['status'] = 'passed'
                record['compiler_crash_retried'] = retried
            except Failed as failure:
                result, detail = f'FAIL: {failure}', failure.detail
                entry['warned'] = failure.warned
                record['status'] = 'failed'
                record['failure_reason'] = str(failure)
                record['diagnostic_excerpt'] = failure.detail or None
                if GROUPS:
                    print(f'::error::{check.name}: {failure}', flush=True)
            elapsed = time.monotonic() - started
            seconds = f'{elapsed:.0f} s'
            execution['finished_at'] = utc_now()
            record['duration_seconds'] = round(elapsed, 3)
            record['execution'] = execution
            if (OUT / f'{check.name}.log').is_file():
                record['log_path'] = str((OUT / f'{check.name}.log').resolve())
            if record['status'] == 'passed':
                entry['execution'] = execution
                entry['compiler_crash_retried'] = retried
            state[check.name] = entry
            state_file.write_text(json.dumps(state, indent=1))
        elif result == 'pass (unchanged)':
            previous = state.get(check.name, {})
            record['status'] = 'reused'
            record['execution'] = previous.get('execution')
            record['compiler_crash_retried'] = previous.get('compiler_crash_retried')
            if (OUT / f'{check.name}.log').is_file():
                record['log_path'] = str((OUT / f'{check.name}.log').resolve())
        elif result.startswith('UNAVAILABLE'):
            record['status'] = 'unavailable'
            record['failure_reason'] = result.partition(': ')[2]
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
    report['finished_at'] = utc_now()
    report['status'] = 'failed' if bad else 'passed'
    report['full_gate_passed'] = args.mode == 'full' and not left_out and not bad
    write_report(report)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
