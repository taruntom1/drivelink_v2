#!/usr/bin/env python3
"""Build the Windows release, stage it and wrap it in the Inno Setup installer.

    python tools/package_win.py [--skip-build] [--no-installer] [--out DIR]
                                [--jobs N] [--iscc PATH]

Steps: release build (Qt MinGW kit), stage (exes, windeployqt, runtime DLLs,
license and notices), notices coverage (every staged DLL is in
host/gui/notices/notices.json), a self-contained smoke with Qt and MinGW off
PATH, then ISCC on host/gui/installer/drivelink.iss. Exit 0 ok, 1 failure,
2 missing prerequisite. Standard library only.
"""
import argparse
import fnmatch
import json
import os
from pathlib import Path
import shutil
import sys

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import version  # noqa: E402
from package_common import Failure, Unavailable, run, say, smoke as smoke_programs  # noqa: E402

QT_ROOT = Path('C:/Qt/6.12.0/mingw_64')
QT_TOOLS = Path('C:/Qt/Tools')
TOOL_DIRS = [QT_TOOLS / 'mingw1310_64/bin', QT_TOOLS / 'CMake_64/bin', QT_TOOLS / 'Ninja']
MINGW_RUNTIME = ['libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll']
PRESET = 'windows-mingw-release'
SUPERBUILD = ROOT / 'host/superbuild'
BUILD = SUPERBUILD / 'build' / PRESET
EXES = {'drivelink-gui.exe': 'gui', 'drivelink-cli.exe': 'cli', 'drivelink-vdev.exe': 'vdev'}
QML_DIRS = ['host/gui/app', 'host/gui/style', 'host/gui/modules']
# The GUI sets the Basic style. windeployqt takes every optional style of
# QtQuick.Controls (and NativeStyle for Windows' own); none of them is loaded.
UNUSED_STYLES = ['FluentWinUI3', 'Fusion', 'Imagine', 'Material', 'Universal', 'Windows']
NOTICES_DIR = ROOT / 'host/gui/notices'
SCRIPT = ROOT / 'host/gui/installer/drivelink.iss'
INSTALL_HINT = 'winget install --id JRSoftware.InnoSetup -e'


# Pure helpers

def staged_dlls(stage):
    """Every .dll under stage (any depth), as sorted stage-relative POSIX paths."""
    stage = Path(stage)
    return sorted(p.relative_to(stage).as_posix() for p in stage.rglob('*')
                  if p.is_file() and p.suffix.lower() == '.dll')


def unmapped(files, manifest):
    """The .dll files whose name matches no dll_patterns glob of any component."""
    components = manifest['components'] if isinstance(manifest, dict) else manifest
    patterns = [pattern.lower() for item in components for pattern in item.get('dll_patterns', [])]
    missing = []
    for path in files:
        name = str(path).replace('\\', '/').rsplit('/', 1)[-1].lower()
        if name.endswith('.dll') and not any(fnmatch.fnmatchcase(name, p) for p in patterns):
            missing.append(str(path))
    return sorted(missing)


def clean_env(base, system_root=None):
    """base without Qt/QML variables and with PATH reduced to the Windows directories."""
    root = system_root or base.get('SystemRoot') or base.get('SYSTEMROOT') or 'C:\\Windows'
    env = {key: value for key, value in base.items()
           if not key.upper().startswith(('QT_', 'QML_', 'QML2_'))}
    for key in [k for k in env if k.upper() == 'PATH']:
        del env[key]
    env['PATH'] = f'{root}\\System32;{root}'
    return env


def find_iscc(explicit=None, which=shutil.which, environ=os.environ, is_file=os.path.isfile):
    """ISCC.exe from --iscc, then PATH, then the per-user and system Inno Setup 6 folders."""
    if explicit:
        return str(explicit) if is_file(explicit) else None
    found = which('ISCC')
    if found:
        return found
    for variable in ('LOCALAPPDATA', 'ProgramFiles(x86)', 'ProgramFiles'):
        base = environ.get(variable)
        if not base:
            continue
        parent = 'Programs/Inno Setup 6' if variable == 'LOCALAPPDATA' else 'Inno Setup 6'
        candidate = f'{base}/{parent}/ISCC.exe'
        if is_file(candidate):
            return candidate
    return None


def unused_style_files(stage):
    """Stage paths of the Controls styles the GUI never loads (and NativeStyle)."""
    stage = Path(stage)
    found = [stage / 'qml/QtQuick/Controls' / style for style in UNUSED_STYLES]
    found += [stage / 'qml/QtQuick/Controls/Native', stage / 'qml/QtQuick/NativeStyle',
              stage / 'Qt6QuickNativeStyle.dll']
    found += [p for style in UNUSED_STYLES for p in stage.glob(f'Qt6QuickControls2{style}*.dll')]
    return [path for path in found if path.exists()]


def output_name(full_version):
    return f'DriveLink-{full_version}-setup.exe'


def tool_path(env_path=None):
    """PATH with the Qt tool directories first."""
    return ';'.join([*map(str, TOOL_DIRS), env_path if env_path is not None else os.environ['PATH']])


# Steps

def check_tools():
    missing = [str(d) for d in TOOL_DIRS if not d.is_dir()]
    windeploy = QT_ROOT / 'bin/windeployqt.exe'
    if not windeploy.is_file():
        missing.append(str(windeploy))
    if missing:
        raise Unavailable('Qt 6.12.0 MinGW kit not found: ' + ', '.join(missing))


def build(out, jobs):
    env = dict(os.environ, PATH=tool_path())
    gpp = TOOL_DIRS[0] / 'g++.exe'
    found = run([gpp, '-dumpfullversion'], env=env).stdout.strip()
    if found != '13.1.0':
        raise Unavailable(f'the release build needs Qt MinGW GCC 13.1.0, found {found!r}')
    log = out / 'build.log'
    for argv in (['cmake', '--preset', PRESET],
                 ['cmake', '--build', '--preset', PRESET, '--parallel', str(jobs)]):
        if run(argv, env=env, cwd=SUPERBUILD, log=log).returncode != 0:
            tail = '\n'.join(log.read_text(encoding='utf-8', errors='replace').splitlines()[-15:])
            raise Failure(f'{" ".join(argv[:2])} failed, see {log}\n{tail}')
    say(f'build: {PRESET} ok')


def stage(out, build_dir=BUILD):
    """Wipe and fill out/stage; return its path."""
    target = out / 'stage'
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir(parents=True)
    for exe, folder in EXES.items():
        source = build_dir / folder / exe
        if not source.is_file():
            raise Failure(f'missing build output {source}; build first (omit --skip-build)')
        shutil.copy2(source, target / exe)
    command = [QT_ROOT / 'bin/windeployqt.exe', '--release', '--compiler-runtime',
               '--no-translations', '--no-system-d3d-compiler', '--no-system-dxc-compiler',
               '--no-opengl-sw', '--skip-plugin-types', 'qmltooling,generic',
               '--include-plugins', 'qoffscreen', '--dir', target]  # qoffscreen: --out screenshots
    for folder in QML_DIRS:
        command += ['--qmldir', ROOT / folder]
    command.append(target / 'drivelink-gui.exe')
    env = dict(os.environ, PATH=tool_path())
    done = run(command, env=env, log=out / 'windeployqt.log')
    if done.returncode != 0:
        raise Failure(f'windeployqt failed, see {out / "windeployqt.log"}')
    offscreen = target / 'platforms/qoffscreen.dll'  # --out screenshots and the smoke need it
    if not offscreen.is_file():
        (target / 'platforms').mkdir(exist_ok=True)
        shutil.copy2(QT_ROOT / 'plugins/platforms/qoffscreen.dll', offscreen)
    for path in unused_style_files(target):
        shutil.rmtree(path) if path.is_dir() else path.unlink()
    for name in MINGW_RUNTIME:
        if not (target / name).is_file():
            source = TOOL_DIRS[0] / name
            if not source.is_file():
                raise Failure(f'MinGW runtime {name} not found in {TOOL_DIRS[0]}')
            shutil.copy2(source, target / name)
    shutil.copy2(ROOT / 'LICENSE', target / 'LICENSE.txt')
    notices = NOTICES_DIR / 'THIRD_PARTY_NOTICES.txt'
    shutil.copy2(notices, target / notices.name)
    say(f'stage: {sum(1 for p in target.rglob("*") if p.is_file())} files, '
        f'{len(staged_dlls(target))} DLLs in {target}')
    return target


def check_notices(target):
    manifest = json.loads((NOTICES_DIR / 'notices.json').read_text(encoding='utf-8'))
    missing = unmapped(staged_dlls(target), manifest)
    if missing:
        raise Failure('staged DLLs without a notices.json dll_patterns entry:\n  '
                      + '\n  '.join(missing))
    say(f'notices: all {len(staged_dlls(target))} staged DLLs are covered')


def smoke(target):
    """Run the staged programs with Qt and MinGW off PATH, from a scratch directory."""
    # Never start the GUI without the offscreen platform: a WIN32 program's
    # fatal Qt error is a modal message box nobody would close.
    if not (target / 'platforms/qoffscreen.dll').is_file():
        raise Failure('platforms/qoffscreen.dll is not staged; the GUI smoke would open a message box')
    smoke_programs(target, dict(clean_env(os.environ), QT_QPA_PLATFORM='offscreen'), suffix='.exe')


def installer(target, out, iscc, full):
    iscc = find_iscc(iscc)
    if not iscc:
        raise Unavailable(f'Inno Setup 6 (ISCC.exe) not found; install it with: {INSTALL_HINT}'
                          ' (or pass --iscc PATH)')
    done = run([iscc, f'/DAppVersion={full}', f'/DStageDir={target}', f'/DRepoRoot={ROOT}',
                f'/O{out}', '/Q', SCRIPT], log=out / 'iscc.log')
    result = out / output_name(full)
    if done.returncode != 0 or not result.is_file():
        log = (out / 'iscc.log').read_text(encoding='utf-8', errors='replace')
        raise Failure(f'ISCC failed (exit {done.returncode}), see {out / "iscc.log"}\n'
                      + '\n'.join(log.splitlines()[-15:]))
    say(f'installer: {result} ({result.stat().st_size / 1048576:.1f} MiB)')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--skip-build', action='store_true', help='reuse the existing release build')
    parser.add_argument('--no-installer', action='store_true', help='stop after the staged smoke')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/installer')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    parser.add_argument('--iscc', help='path to ISCC.exe')
    args = parser.parse_args(argv)
    out = args.out.resolve()
    try:
        if sys.platform != 'win32':
            raise Unavailable('the Windows package is built on Windows')
        check_tools()
        if not args.no_installer and not find_iscc(args.iscc):
            raise Unavailable(f'Inno Setup 6 (ISCC.exe) not found; install it with: {INSTALL_HINT}'
                              ' (or pass --iscc PATH, or --no-installer)')
        full = version.full()
        out.mkdir(parents=True, exist_ok=True)
        say(f'DriveLink {full}')
        if not args.skip_build:
            build(out, args.jobs)
        target = stage(out)
        check_notices(target)
        smoke(target)
        if not args.no_installer:
            installer(target, out, args.iscc, full)
    except Unavailable as error:
        print(f'package_win: unavailable: {error}', file=sys.stderr)
        return 2
    except Failure as error:
        print(f'package_win: failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
