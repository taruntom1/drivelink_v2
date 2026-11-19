#!/usr/bin/env python3
"""Build the Linux release and wrap the GUI, CLI and vdev in a .deb.

    python3 tools/package_linux.py [--skip-build] [--build-dir DIR] [--out DIR] [--jobs N]

Steps: release build (preset linux-release, Ubuntu's Qt 6), stage under /usr
(stripped programs, desktop entry, licence and notices), a check that nothing
bundles a shared library (Qt and the C++ runtime come from Ubuntu's packages,
named in Depends), a smoke of the staged programs, then dpkg-deb. Depends is
dpkg-shlibdeps' list plus the QML modules the GUI imports. The package targets
the Ubuntu release it is built on (26.04). Exit 0 ok, 1 failure, 2 missing
prerequisite. Standard library only.

The stage lives in the build directory (a Linux filesystem: dpkg-deb checks
file modes, which a Windows drive mounted in WSL cannot keep).
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import version  # noqa: E402
from package_common import Failure, Unavailable, run, run_logged, say, smoke  # noqa: E402

PRESET = 'linux-release'
SUPERBUILD = ROOT / 'host/superbuild'
PROGRAMS = {'drivelink-gui': 'gui/app', 'drivelink-cli': 'cli', 'drivelink-vdev': 'vdev'}
PACKAGE = 'drivelink'
# Runtime QML modules the GUI imports; dpkg-shlibdeps cannot see them.
QML_PACKAGES = ['qml6-module-qtqml', 'qml6-module-qtqml-models', 'qml6-module-qtqml-workerscript',
                'qml6-module-qtcore', 'qml6-module-qtquick', 'qml6-module-qtquick-controls',
                'qml6-module-qtquick-templates', 'qml6-module-qtquick-layouts',
                'qml6-module-qtquick-window', 'qml6-module-qtquick-dialogs']
RECOMMENDS = ['qt6-wayland']  # the GUI on a Wayland session; xcb is in libqt6gui6
NOTICES = ROOT / 'host/gui/notices/THIRD_PARTY_NOTICES.txt'
MAINTAINER = 'DriveLink maintainers <taruntom1@users.noreply.github.com>'  # package.xml's
HOMEPAGE = 'https://github.com/taruntom1/drivelink_v2'
TOOLS_NEEDED = ['cmake', 'ninja', 'g++', 'strip', 'dpkg-deb', 'dpkg-shlibdeps', 'dpkg']
DESKTOP = """\
[Desktop Entry]
Type=Application
Name=DriveLink
GenericName=Motion controller setup
Comment=Configure, tune and monitor a DriveLink device
Exec=drivelink-gui
Terminal=false
Categories=Development;Electronics;
"""
DESCRIPTION = """\
DriveLink host tools for an ESP32-S3 motion controller
 drivelink-gui configures, tunes and monitors a DriveLink device over USB
 serial or TCP. drivelink-cli is the command line client and drivelink-vdev
 a virtual device for trying both without hardware."""


# Pure helpers

def debian_version(full):
    """The package version: VERSION's full form, which Debian accepts as is."""
    if not re.fullmatch(r'[0-9][A-Za-z0-9.+~]*', full):
        raise Failure(f'{full!r} is not a valid Debian version')
    return full


def deb_name(full, arch):
    return f'{PACKAGE}_{debian_version(full)}_{arch}.deb'


def parse_shlibdeps(output):
    """The packages of dpkg-shlibdeps -O output ('shlibs:Depends=a (>= 1), b')."""
    for line in output.splitlines():
        if line.startswith('shlibs:Depends='):
            return [item.strip() for item in line.split('=', 1)[1].split(',') if item.strip()]
    raise Failure(f'dpkg-shlibdeps printed no shlibs:Depends line:\n{output.strip()}')


def merge_depends(shlibs, extra):
    """shlibs first, then each extra package not already named (by package name)."""
    named = {item.split()[0].split(':')[0] for item in shlibs}
    return [*shlibs, *(item for item in extra if item not in named)]


def control(full, arch, depends, installed_kib):
    fields = [('Package', PACKAGE), ('Version', debian_version(full)), ('Architecture', arch),
              ('Maintainer', MAINTAINER), ('Installed-Size', str(installed_kib)),
              ('Depends', ', '.join(depends)), ('Recommends', ', '.join(RECOMMENDS)),
              ('Section', 'electronics'), ('Priority', 'optional'), ('Homepage', HOMEPAGE),
              ('Description', DESCRIPTION)]
    return ''.join(f'{key}: {value}\n' for key, value in fields)


def lf(text):
    return text.replace('\r\n', '\n')


def shared_libraries(stage):
    """Stage-relative paths of every shared library (none may be bundled)."""
    stage = Path(stage)
    return sorted(p.relative_to(stage).as_posix() for p in stage.rglob('*')
                  if p.is_file() and re.search(r'\.so(\.\d+)*$', p.name))


# Steps

def check_tools():
    if not sys.platform.startswith('linux'):
        raise Unavailable('the .deb is built on Linux (validate.py runs it in WSL on Windows)')
    missing = [tool for tool in TOOLS_NEEDED if not shutil.which(tool)]
    if missing:
        raise Unavailable('missing ' + ', '.join(missing) + '; run: sudo sh tools/setup_linux.sh host')


def build(build_dir, out, jobs):
    log = out / 'build.log'
    for argv in (['cmake', '--preset', PRESET, '-B', build_dir],
                 ['cmake', '--build', build_dir, '--parallel', str(jobs)]):
        run_logged(argv, log, cwd=SUPERBUILD)
    say(f'build: {PRESET} ok')


def stage(build_dir):
    """Wipe and fill <build_dir>/deb/root; return its path."""
    root = build_dir / 'deb/root'
    shutil.rmtree(root.parent, ignore_errors=True)
    bin_dir = root / 'usr/bin'
    bin_dir.mkdir(parents=True)
    for program, folder in PROGRAMS.items():
        source = build_dir / folder / program
        if not source.is_file():
            raise Failure(f'missing build output {source}; build first (omit --skip-build)')
        shutil.copyfile(source, bin_dir / program)
        run_logged(['strip', '--strip-unneeded', bin_dir / program], build_dir / 'deb/strip.log')
        (bin_dir / program).chmod(0o755)
    docs = root / f'usr/share/doc/{PACKAGE}'
    docs.mkdir(parents=True)
    licence = lf((ROOT / 'LICENSE').read_text(encoding='utf-8'))
    (docs / 'LICENSE.txt').write_text(licence, encoding='utf-8')
    (docs / 'THIRD_PARTY_NOTICES.txt').write_text(lf(NOTICES.read_text(encoding='utf-8')), encoding='utf-8')
    (docs / 'copyright').write_text(
        f'DriveLink\n\n{licence.rstrip()}\n\nThird-party software: see '
        f'/usr/share/doc/{PACKAGE}/THIRD_PARTY_NOTICES.txt. Qt and the C++ runtime are '
        'not bundled; they come from the Ubuntu packages this package depends on.\n',
        encoding='utf-8')
    applications = root / 'usr/share/applications'
    applications.mkdir(parents=True)
    (applications / f'{PACKAGE}.desktop').write_text(DESKTOP, encoding='utf-8')
    for path in root.rglob('*'):
        if path.is_dir():
            path.chmod(0o755)
        elif path.parent != bin_dir:
            path.chmod(0o644)
    root.chmod(0o755)
    say(f'stage: {sum(1 for p in root.rglob("*") if p.is_file())} files in {root}')
    return root


def check_bundled(root):
    found = shared_libraries(root)
    if found:
        raise Failure('the .deb bundles shared libraries, which notices.json does not cover '
                      'for Linux:\n  ' + '\n  '.join(found))
    say('notices: no bundled libraries (Qt and the runtime come from Depends)')


def depends(root):
    programs = [root / 'usr/bin' / name for name in PROGRAMS]
    with tempfile.TemporaryDirectory(prefix='drivelink-shlibdeps-') as scratch:
        (Path(scratch) / 'debian').mkdir()
        (Path(scratch) / 'debian/control').write_text(
            f'Source: {PACKAGE}\n\nPackage: {PACKAGE}\nArchitecture: any\n', encoding='utf-8')
        done = run(['dpkg-shlibdeps', '-O', *(f'-e{p}' for p in programs)], cwd=scratch, timeout=120)
    if done.returncode != 0:
        raise Failure(f'dpkg-shlibdeps failed (exit {done.returncode}):\n{done.stderr.strip()}')
    return merge_depends(parse_shlibdeps(done.stdout), QML_PACKAGES)


def package(root, out, full):
    arch = run(['dpkg', '--print-architecture']).stdout.strip()
    kib = (sum(p.stat().st_size for p in root.rglob('*') if p.is_file()) + 1023) // 1024
    meta = root / 'DEBIAN'
    meta.mkdir(mode=0o755)
    (meta / 'control').write_text(control(full, arch, depends(root), kib), encoding='utf-8')
    (meta / 'control').chmod(0o644)
    result = out / deb_name(full, arch)
    result.unlink(missing_ok=True)
    run_logged(['dpkg-deb', '--root-owner-group', '--build', root, result], out / 'dpkg-deb.log')
    info = run(['dpkg-deb', '--field', result, 'Depends']).stdout.strip()
    say(f'package: {result} ({result.stat().st_size / 1048576:.1f} MiB)\n  Depends: {info}')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--skip-build', action='store_true', help='reuse the existing release build')
    parser.add_argument('--build-dir', type=Path, default=SUPERBUILD / 'build' / PRESET)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/deb')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args(argv)
    out, build_dir = args.out.resolve(), args.build_dir.resolve()
    try:
        check_tools()
        full = version.full()
        out.mkdir(parents=True, exist_ok=True)
        say(f'DriveLink {full}')
        if not args.skip_build:
            build(build_dir, out, args.jobs)
        root = stage(build_dir)
        check_bundled(root)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('QT_', 'QML', 'LD_'))}
        smoke(root / 'usr/bin', dict(env, QT_QPA_PLATFORM='offscreen'))
        package(root, out, full)
    except Unavailable as error:
        print(f'package_linux: unavailable: {error}', file=sys.stderr)
        return 2
    except Failure as error:
        print(f'package_linux: failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
