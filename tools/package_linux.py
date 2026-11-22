#!/usr/bin/env python3
"""Build the Linux release and wrap it in the DriveLink .deb packages.

    python3 tools/package_linux.py [--skip-build] [--build-dir DIR] [--out DIR] [--jobs N]

Packages (H3c), each for the Ubuntu release and architecture it is built on:
  drivelink-cli    drivelink-cli and drivelink-vdev (no Qt)
  drivelink-agent  drivelink-agent, its systemd unit, udev rules, config and
                   maintainer scripts (user, token, service); Recommends drivelink-cli
  drivelink        drivelink-gui; Depends drivelink-cli at the same version.
                   Only where Ubuntu's Qt meets the GUI's floor (6.10: 26.04 on)

The version is VERSION's full form plus the release, `<full>~ubuntu<release>`,
so the builds for two releases differ and the older release sorts lower.
Steps: release build (preset linux-release, or linux-release-no-gui without
the GUI), stage each package under /usr (stripped programs, licence and
notices), a check that nothing bundles a shared library (Qt and the C++
runtime come from Ubuntu's packages, named in Depends), a smoke of the staged
programs (the agent with the shipped config), then dpkg-deb. Depends is
dpkg-shlibdeps' list plus the packages it cannot see. Exit 0 ok, 1 failure,
2 missing prerequisite. Standard library only.

The stage lives in the build directory (a Linux filesystem: dpkg-deb checks
file modes, which a Windows drive mounted in WSL cannot keep).
"""
import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import version  # noqa: E402
from package_common import Failure, Unavailable, read_lines, run, run_logged, say, smoke, start_vdev, stop  # noqa: E402

SUPERBUILD = ROOT / 'host/superbuild'
DEPLOY = ROOT / 'host/agent/deploy'
GUI_FLOOR = (26, 4)  # the first Ubuntu release whose Qt (6.10) meets the GUI's API floor
MAINTAINER = 'DriveLink maintainers <taruntom1@users.noreply.github.com>'  # package.xml's
HOMEPAGE = 'https://github.com/taruntom1/drivelink_v2'
TOOLS_NEEDED = ['cmake', 'ninja', 'g++', 'strip', 'dpkg-deb', 'dpkg-shlibdeps', 'dpkg']
# Runtime QML modules the GUI imports; dpkg-shlibdeps cannot see them.
QML_PACKAGES = ['qml6-module-qtqml', 'qml6-module-qtqml-models', 'qml6-module-qtqml-workerscript',
                'qml6-module-qtcore', 'qml6-module-qtquick', 'qml6-module-qtquick-controls',
                'qml6-module-qtquick-templates', 'qml6-module-qtquick-layouts',
                'qml6-module-qtquick-window', 'qml6-module-qtquick-dialogs']
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


@dataclass
class Package:
    name: str
    programs: dict           # program -> build folder
    description: str
    notices: Path
    depends: list = field(default_factory=list)      # beyond dpkg-shlibdeps
    recommends: list = field(default_factory=list)
    replaces: list = field(default_factory=list)
    files: dict = field(default_factory=dict)        # stage path -> (source, mode)
    conffiles: list = field(default_factory=list)
    scripts: list = field(default_factory=list)      # maintainer scripts in host/agent/deploy
    gui: bool = False
    same_version: list = field(default_factory=list)  # packages Depended on at exactly this version


PACKAGES = [
    Package(
        name='drivelink-cli',
        programs={'drivelink-cli': 'cli', 'drivelink-vdev': 'vdev'},
        description='DriveLink command line client and virtual device\n'
                    ' drivelink-cli configures, monitors and scripts a DriveLink device (an ESP32-S3\n'
                    ' motion controller) over serial, TCP or a drivelink-agent. drivelink-vdev is a\n'
                    ' virtual device for trying it without hardware.',
        notices=ROOT / 'host/notices/THIRD_PARTY_NOTICES.txt',
        # H2g's `drivelink` package held these programs. Its builds and the
        # split ones share VERSION (0.1.0+g<sha>), so no version bound can
        # tell them apart; the new `drivelink` ships none of these files.
        replaces=['drivelink']),
    Package(
        name='drivelink-agent',
        programs={'drivelink-agent': 'agent'},
        description='DriveLink agent: shares DriveLink devices over TCP\n'
                    ' drivelink-agent owns the serial ports of DriveLink devices (ESP32-S3 motion\n'
                    ' controllers) and shares them with the GUI, the CLI and ROS 2, with access\n'
                    ' control and a control lease. It runs as the drivelink-agent service on\n'
                    ' 127.0.0.1 with a generated token until /etc/drivelink/agent.json lists\n'
                    ' devices; an upgrade never restarts the running agent.',
        notices=ROOT / 'host/notices/THIRD_PARTY_NOTICES.txt',
        depends=['passwd'],  # useradd in postinst
        recommends=['drivelink-cli'],
        files={'usr/lib/systemd/system/drivelink-agent.service': (DEPLOY / 'drivelink-agent.service', 0o644),
               'usr/lib/udev/rules.d/70-drivelink.rules': (DEPLOY / '70-drivelink.rules', 0o644),
               'etc/drivelink/agent.json': (DEPLOY / 'agent.json', 0o644)},
        conffiles=['/etc/drivelink/agent.json'],
        scripts=['postinst', 'prerm', 'postrm']),
    Package(
        name='drivelink',
        programs={'drivelink-gui': 'gui/app'},
        description='DriveLink GUI for an ESP32-S3 motion controller\n'
                    ' drivelink-gui configures, tunes and monitors a DriveLink device over USB\n'
                    ' serial, TCP or a drivelink-agent. The command line client and the virtual\n'
                    ' device are in drivelink-cli.',
        notices=ROOT / 'host/gui/notices/THIRD_PARTY_NOTICES.txt',
        depends=QML_PACKAGES,
        recommends=['qt6-wayland'],  # the GUI on a Wayland session; xcb is in libqt6gui6
        files={'usr/share/applications/drivelink.desktop': (DESKTOP, 0o644)},
        gui=True,
        same_version=['drivelink-cli']),
]
BY_NAME = {p.name: p for p in PACKAGES}


# Pure helpers

def debian_version(full):
    """VERSION's full form, which Debian accepts as is."""
    if not re.fullmatch(r'[0-9][A-Za-z0-9.+~]*', full):
        raise Failure(f'{full!r} is not a valid Debian version')
    return full


def parse_os_release(text):
    """(ID, VERSION_ID) from /etc/os-release text."""
    values = {}
    for line in text.splitlines():
        key, sep, value = line.partition('=')
        if sep:
            values[key.strip()] = value.strip().strip('"\'')
    if not values.get('ID') or not re.fullmatch(r'\d+(\.\d+)*', values.get('VERSION_ID', '')):
        raise Failure('/etc/os-release names no ID and numeric VERSION_ID')
    return values['ID'], values['VERSION_ID']


def gui_supported(distro, release):
    return distro == 'ubuntu' and tuple(int(n) for n in release.split('.')) >= GUI_FLOOR


def package_version(full, distro, release):
    """<full>~<distro><release>: 0.1.0+gabc1234~ubuntu24.04 sorts below ~ubuntu26.04."""
    return f'{debian_version(full)}~{distro}{release}'


def packages_for(distro, release):
    return [p for p in PACKAGES if not p.gui or gui_supported(distro, release)]


def deb_name(package, pkg_version, arch):
    return f'{package}_{pkg_version}_{arch}.deb'


def parse_shlibdeps(output):
    """The packages of dpkg-shlibdeps -O output ('shlibs:Depends=a (>= 1), b')."""
    for line in output.splitlines():
        if line.startswith('shlibs:Depends='):
            return [item.strip() for item in line.split('=', 1)[1].split(',') if item.strip()]
    raise Failure(f'dpkg-shlibdeps printed no shlibs:Depends line:\n{output.strip()}')


def merge_depends(shlibs, extra):
    """shlibs first, then each extra package not already named (by package name)."""
    named = {item.split()[0].split(':')[0] for item in shlibs}
    return [*shlibs, *(item for item in extra if item.split()[0] not in named)]


def all_depends(package, shlibs, pkg_version):
    exact = [f'{name} (= {pkg_version})' for name in package.same_version]
    return merge_depends(shlibs, [*exact, *package.depends])


def control(package, pkg_version, arch, depends, installed_kib):
    fields = [('Package', package.name), ('Version', pkg_version), ('Architecture', arch),
              ('Maintainer', MAINTAINER), ('Installed-Size', str(installed_kib)),
              ('Depends', ', '.join(depends))]
    if package.recommends:
        fields.append(('Recommends', ', '.join(package.recommends)))
    if package.replaces:
        fields.append(('Replaces', ', '.join(package.replaces)))
    fields += [('Section', 'electronics'), ('Priority', 'optional'), ('Homepage', HOMEPAGE),
               ('Description', package.description)]
    return ''.join(f'{key}: {value}\n' for key, value in fields)


def maintainer_script(text, pkg_version):
    if '@VERSION@' in text and not re.fullmatch(r'[A-Za-z0-9.+~]+', pkg_version):
        raise Failure(f'{pkg_version!r} cannot go into a maintainer script')
    return lf(text).replace('@VERSION@', pkg_version)


def lf(text):
    return text.replace('\r\n', '\n')


def shared_libraries(stage):
    """Stage-relative paths of every shared library (none may be bundled)."""
    stage = Path(stage)
    return sorted(p.relative_to(stage).as_posix() for p in stage.rglob('*')
                  if p.is_file() and re.search(r'\.so(\.\d+)*$', p.name))


# Steps

def os_release():
    return parse_os_release(Path('/etc/os-release').read_text(encoding='utf-8'))


def check_tools(profile):
    if not sys.platform.startswith('linux'):
        raise Unavailable('the .deb is built on Linux (validate.py runs it in WSL on Windows)')
    missing = [tool for tool in TOOLS_NEEDED if not shutil.which(tool)]
    if missing:
        raise Unavailable('missing ' + ', '.join(missing) + f'; run: sudo sh tools/setup_linux.sh {profile}')


def build(preset, build_dir, out, jobs):
    log = out / 'build.log'
    for argv in (['cmake', '--preset', preset, '-B', build_dir],
                 ['cmake', '--build', build_dir, '--parallel', str(jobs)]):
        run_logged(argv, log, cwd=SUPERBUILD)
    say(f'build: {preset} ok')


def write(path, content, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding='utf-8')
    path.chmod(mode)


def stage(package, build_dir, pkg_version):
    """Wipe and fill <build_dir>/deb/<package>; return its path."""
    root = build_dir / 'deb' / package.name
    shutil.rmtree(root, ignore_errors=True)
    bin_dir = root / 'usr/bin'
    bin_dir.mkdir(parents=True)
    for program, folder in package.programs.items():
        source = build_dir / folder / program
        if not source.is_file():
            raise Failure(f'missing build output {source}; build first (omit --skip-build)')
        shutil.copyfile(source, bin_dir / program)
        run_logged(['strip', '--strip-unneeded', bin_dir / program], build_dir / 'deb/strip.log')
        (bin_dir / program).chmod(0o755)
    # The licence and notices live in /usr/share/<package>: minimised systems
    # (Ubuntu's container images) drop /usr/share/doc except `copyright`.
    licence = lf((ROOT / 'LICENSE').read_text(encoding='utf-8'))
    data = root / f'usr/share/{package.name}'
    write(data / 'LICENSE.txt', licence, 0o644)
    write(data / 'THIRD_PARTY_NOTICES.txt', lf(package.notices.read_text(encoding='utf-8')), 0o644)
    write(root / f'usr/share/doc/{package.name}/copyright',
          f'DriveLink\n\n{licence.rstrip()}\n\nThird-party software: see '
          f'/usr/share/{package.name}/THIRD_PARTY_NOTICES.txt. Qt and the C++ runtime are '
          'not bundled; they come from the Ubuntu packages this package depends on.\n', 0o644)
    for target, (source, mode) in package.files.items():
        text = source if isinstance(source, str) else source.read_text(encoding='utf-8')
        write(root / target, lf(text), mode)
    meta = root / 'DEBIAN'
    meta.mkdir()
    for script in package.scripts:
        write(meta / script, maintainer_script((DEPLOY / script).read_text(encoding='utf-8'), pkg_version), 0o755)
    if package.conffiles:
        write(meta / 'conffiles', ''.join(f'{path}\n' for path in package.conffiles), 0o644)
    for path in root.rglob('*'):
        if path.is_dir():
            path.chmod(0o755)
    root.chmod(0o755)
    say(f'stage: {package.name}: {sum(1 for p in root.rglob("*") if p.is_file())} files')
    return root


def check_bundled(roots):
    found = [f'{root.name}/{path}' for root in roots for path in shared_libraries(root)]
    if found:
        raise Failure('a .deb bundles shared libraries, which the notices do not cover '
                      'for Linux:\n  ' + '\n  '.join(found))
    say('notices: no bundled libraries (Qt and the runtime come from Depends)')


def shlibs(root, package):
    programs = [root / 'usr/bin' / name for name in package.programs]
    with tempfile.TemporaryDirectory(prefix='drivelink-shlibdeps-') as scratch:
        (Path(scratch) / 'debian').mkdir()
        (Path(scratch) / 'debian/control').write_text(
            f'Source: {package.name}\n\nPackage: {package.name}\nArchitecture: any\n', encoding='utf-8')
        done = run(['dpkg-shlibdeps', '-O', *(f'-e{p}' for p in programs)], cwd=scratch, timeout=120)
    if done.returncode != 0:
        raise Failure(f'dpkg-shlibdeps failed (exit {done.returncode}):\n{done.stderr.strip()}')
    return parse_shlibdeps(done.stdout)


def make_deb(package, root, out, pkg_version, arch):
    kib = (sum(p.stat().st_size for p in root.rglob('*') if p.is_file() and p.parent.name != 'DEBIAN')
           + 1023) // 1024
    depends = all_depends(package, shlibs(root, package), pkg_version)
    write(root / 'DEBIAN/control', control(package, pkg_version, arch, depends, kib), 0o644)
    result = out / deb_name(package.name, pkg_version, arch)
    result.unlink(missing_ok=True)
    run_logged(['dpkg-deb', '--root-owner-group', '--build', root, result], out / 'dpkg-deb.log')
    info = run(['dpkg-deb', '--field', result, 'Depends']).stdout.strip()
    say(f'package: {result.name} ({result.stat().st_size / 1048576:.1f} MiB)\n  Depends: {info}')
    return result


class Agent:
    """A staged drivelink-agent with a READY line."""

    def __init__(self, argv, env):
        self.process = subprocess.Popen([str(a) for a in argv], env=env, cwd=tempfile.gettempdir(),
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                        errors='replace')
        self.lines = queue.Queue()
        self.seen = []
        threading.Thread(target=read_lines, args=(self.process.stdout, self.lines), daemon=True).start()

    def ready(self, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                break
            self.seen.append(line)
            if line.startswith('READY '):
                return line.split(None, 1)[1]
        stop(self.process)
        raise Failure('drivelink-agent did not print READY:\n  ' + '\n  '.join(self.seen[-10:]))

    def terminate(self):
        self.process.terminate()
        try:
            return self.process.wait(15)
        except subprocess.TimeoutExpired as error:
            stop(self.process)
            raise Failure('drivelink-agent did not stop on SIGTERM') from error


def agent_devices(cli, url, token, env):
    done = run([cli, '--endpoint', url, '--token', token, '--json', 'agent-devices'], env=env, timeout=30)
    if done.returncode != 0:
        raise Failure(f'drivelink-cli agent-devices exited {done.returncode}: {done.stderr.strip()}')
    return json.loads(done.stdout.strip().splitlines()[-1])['data']['devices']


def smoke_agent(agent_root, cli_root, env):
    """The staged agent with the shipped config (no devices), then with the staged vdev."""
    agent = agent_root / 'usr/bin/drivelink-agent'
    cli = cli_root / 'usr/bin/drivelink-cli'
    done = run([agent, '--version'], env=env, timeout=30)
    if done.returncode != 0 or not done.stdout.startswith('drivelink-agent '):
        raise Failure(f'drivelink-agent --version: exit {done.returncode}, {done.stdout.strip()!r}')
    done = run([agent, '--list-ports'], env=env, timeout=30)
    if done.returncode not in (0, 1):
        raise Failure(f'drivelink-agent --list-ports exited {done.returncode}: {done.stderr.strip()}')
    with tempfile.TemporaryDirectory(prefix='drivelink-agent-smoke-') as scratch:
        config = Path(scratch) / 'agent.json'
        shutil.copyfile(agent_root / 'etc/drivelink/agent.json', config)
        token = 'staged-smoke-' + os.urandom(8).hex()
        (Path(scratch) / 'token').write_text(token + '\n', encoding='utf-8')
        base = [agent, '--config', config, '--port', '0']
        empty = Agent(base, env)
        try:
            url = empty.ready()
            devices = agent_devices(cli, url, token, env)
            if devices != []:
                raise Failure(f'the shipped config should list no devices, the agent listed {devices}')
        finally:
            code = empty.terminate()
        if code != 0:
            raise Failure(f'drivelink-agent exited {code} on SIGTERM')
        say('smoke: agent with the shipped config listed no devices and stopped on SIGTERM')
        vdev, endpoint = start_vdev(cli_root / 'usr/bin/drivelink-vdev', env)
        try:
            agent_proc = Agent([*base, '--device', f'bench={endpoint}', '--retry-ms', '300'], env)
            try:
                url = agent_proc.ready()
                deadline = time.monotonic() + 20
                while True:
                    devices = agent_devices(cli, url, token, env)
                    if devices and devices[0].get('online'):
                        break
                    if time.monotonic() > deadline:
                        raise Failure(f'the agent never brought vdev online: {devices}')
                    time.sleep(0.3)
            finally:
                agent_proc.terminate()
        finally:
            stop(vdev)
        say('smoke: agent brought the staged vdev online')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--skip-build', action='store_true', help='reuse the existing release build')
    parser.add_argument('--build-dir', type=Path, help='default: host/superbuild/build/<preset>')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/deb')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args(argv)
    try:
        distro, release = os_release() if sys.platform.startswith('linux') else ('ubuntu', '26.04')
        gui = gui_supported(distro, release)
        check_tools('host' if gui else 'agent')
        preset = 'linux-release' if gui else 'linux-release-no-gui'
        build_dir = (args.build_dir or SUPERBUILD / 'build' / preset).resolve()
        out = args.out.resolve()
        full = version.full()
        pkg_version = package_version(full, distro, release)
        arch = run(['dpkg', '--print-architecture']).stdout.strip()
        packages = packages_for(distro, release)
        out.mkdir(parents=True, exist_ok=True)
        for stale in out.glob('*.deb'):
            stale.unlink()
        names = ', '.join(p.name for p in packages)
        note = '' if gui else f' (no GUI: the Qt of Ubuntu {release} is below 6.10)'
        say(f'DriveLink {full}: {names} {pkg_version} {arch}{note}')
        if not args.skip_build:
            build(preset, build_dir, out, args.jobs)
        roots = {p.name: stage(p, build_dir, pkg_version) for p in packages}
        check_bundled(roots.values())
        env = {k: v for k, v in os.environ.items() if not k.startswith(('QT_', 'QML', 'LD_'))}
        with tempfile.TemporaryDirectory(prefix='drivelink-smoke-bin-') as scratch:
            # The staged programs side by side, as installed in /usr/bin.
            for root in roots.values():
                for program in (root / 'usr/bin').iterdir():
                    os.symlink(program, Path(scratch) / program.name)
            smoke(scratch, dict(env, QT_QPA_PLATFORM='offscreen'), gui=gui)
        smoke_agent(roots['drivelink-agent'], roots['drivelink-cli'], env)
        for package in packages:
            make_deb(package, roots[package.name], out, pkg_version, arch)
    except Unavailable as error:
        print(f'package_linux: unavailable: {error}', file=sys.stderr)
        return 2
    except Failure as error:
        print(f'package_linux: failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
