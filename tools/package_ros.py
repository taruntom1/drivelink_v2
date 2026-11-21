#!/usr/bin/env python3
"""Build Bloom Debian packages in a disposable ROS build container."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from apt_repo import RepoError, SUITES, parse_control, read_deb
from package_common import Failure, run, say
from package_linux import deb_name, os_release, package_version
from ros_packages import ROS_PACKAGES, ROS_TARGETS, debian_name
import version

ROOT = Path(__file__).resolve().parents[1]


def call(*argv, cwd=None, env=None):
    try:
        subprocess.run([str(arg) for arg in argv], cwd=cwd, env=env, check=True, timeout=1800)
    except (OSError, subprocess.SubprocessError) as error:
        raise Failure(str(error)) from error


def build_deb(package, env):
    """Keep the compiler warning gate while preserving dpkg's ROS diagnostics."""
    command = ['dpkg-buildpackage', '--build=binary', '--no-sign']
    with tempfile.TemporaryDirectory(prefix='drivelink-dpkg-') as tmp:
        log = Path(tmp) / 'build.log'
        result = run(command, cwd=package, env=env, log=log, timeout=1800)
        for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
            # ROS declares its unversioned DSOs in package.xml. Ubuntu's libc
            # loader diversion still resolves to libc6 in the substvars.
            unversioned = re.fullmatch(r"dpkg-shlibdeps: warning: (?:can't|cannot) extract name and "
                                       r"version from library name 'lib[^']+\.so'", line)
            if unversioned or line == 'dpkg-shlibdeps: warning: diversions involved - output may be incorrect':
                line = line.replace('warning:', 'note:', 1)
            print(line)
    if result.returncode:
        raise Failure(f'dpkg-buildpackage exited {result.returncode} for {package.name}')


def rosdep_environment(work, distro, codename):
    sources = work / 'rosdep'
    sources.mkdir()
    rules = work / 'drivelink.yaml'
    rules.write_text(''.join(f'{name}:\n  ubuntu:\n    {codename}: [{debian_name(distro, name)}]\n'
                             for name in ROS_PACKAGES), encoding='utf-8')
    (sources / '00-drivelink.list').write_text(f'yaml {rules.as_uri()}\n', encoding='utf-8')
    env = dict(os.environ, ROSDEP_SOURCE_PATH=f'{sources}:/etc/ros/rosdep/sources.list.d',
               ROS_HOME=str(work / 'ros-home'), ROS_DISTRO=distro, ROS_VERSION='2',
               ROS_PYTHON_VERSION='3', BLOOM_SKIP_ROSDEP_UPDATE='1', DEBIAN_FRONTEND='noninteractive')
    call('rosdep', 'update', '--rosdistro', distro, env=env)
    return env


def fetch_dependencies(work):
    """Use the same URLs and SHA256 pins as the source build."""
    source = work / 'fetch'
    source.mkdir()
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(fetch_dependencies NONE)\n'
        f'include("{ROOT}/host/cmake/Dependencies.cmake")\n', encoding='utf-8')
    deps = work / 'deps'
    call('cmake', '-S', source, '-B', work / 'fetch-build', f'-DFETCHCONTENT_BASE_DIR={deps}')
    for name, header in (('asio', 'asio/include/asio.hpp'), ('mdns', 'mdns.h')):
        if not (deps / f'{name}-src' / header).is_file():
            raise Failure(f'{name}: verified source is missing {header}')
    return deps


def stage(work, relative):
    """Keep the package in the layout its CMakeLists.txt expects."""
    tree = work / Path(relative).name
    package = tree / relative
    shutil.copytree(ROOT / relative, package,
                    ignore=shutil.ignore_patterns('build', '__pycache__'))
    shutil.copy2(ROOT / 'VERSION', tree)
    (tree / 'cmake').mkdir()
    shutil.copy2(ROOT / 'cmake/DriveLinkVersion.cmake', tree / 'cmake')
    if relative == 'host/libdrivelink':
        (tree / 'host/cmake').mkdir()
        shutil.copy2(ROOT / 'host/cmake/Dependencies.cmake', tree / 'host/cmake')
    return package


def pin_dependencies(control, dependencies, pkg_version):
    """Constrain Bloom's internal package dependencies, preserving external ones."""
    for name in dependencies:
        control, count = re.subn(rf'(?<=[: ,]){re.escape(name)}(?=,|\n|$)',
                                 f'{name} (= {pkg_version})', control)
        if not count:
            raise Failure(f'Bloom did not add the dependency on {name}')
    return control


def configure_debian(package, logical, distro, pkg_version, dependencies, deps):
    debian = package / 'debian'
    changelog = debian / 'changelog'
    text = changelog.read_text(encoding='utf-8')
    text, count = re.subn(r'^(\S+ \()[^)]+(\))', rf'\g<1>{pkg_version}\2', text, count=1)
    if not count:
        raise Failure('Bloom changelog has no version to replace')
    changelog.write_text(text, encoding='utf-8')

    control = debian / 'control'
    control_text = pin_dependencies(control.read_text(encoding='utf-8'),
                                    [debian_name(distro, name) for name in dependencies], pkg_version)
    # Bloom's default compatibility level 9 is deprecated on both Ubuntu targets.
    (debian / 'compat').unlink()
    control_text = re.sub(r'(?<=Build-Depends: )debhelper \([^)]+\)', 'debhelper-compat (= 13)', control_text)
    if logical in ('drivelink_protocol', 'drivelink'):
        # The two archives contain no dynamic links for dh_shlibdeps to inspect.
        control_text = control_text.replace('${shlibs:Depends}, ', '')
    control.write_text(control_text, encoding='utf-8')
    shutil.copy2(ROOT / 'LICENSE', debian / 'copyright')

    if logical != 'drivelink_protocol':
        # Asio code is linked into all three of these packages.
        shutil.copy2(ROOT / 'host/notices/THIRD_PARTY_NOTICES.txt', package)
        name = debian_name(distro, logical)
        (debian / 'install').write_text(f'THIRD_PARTY_NOTICES.txt usr/share/{name}\n', encoding='utf-8')

    rules = debian / 'rules'
    text = rules.read_text(encoding='utf-8')
    configure = '$(BUILD_TESTING_ARG)'
    if text.count(configure) != 1:
        raise Failure('Bloom did not generate the expected CMake configure rule')
    # Debhelper supplies CMake options some of the four projects never read.
    options = '--no-warn-unused-cli -DCMAKE_COMPILE_WARNING_AS_ERROR=ON'
    if logical == 'drivelink':
        options += (f' -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_ASIO={deps}/asio-src'
                    f' -DFETCHCONTENT_SOURCE_DIR_MDNS={deps}/mdns-src')
    rules.write_text(text.replace(configure, f'{configure} {options}'), encoding='utf-8')
    return dict(parse_control(control_text.split('\n\n', 1)[0]))


def build_packages(out, jobs):
    os_name, release = os_release()
    distro = ROS_TARGETS.get(release)
    if os_name != 'ubuntu' or not distro or not (Path('/opt/ros') / distro / 'setup.sh').is_file():
        raise Failure('use a ROS Jazzy (Ubuntu 24.04) or Lyrical (Ubuntu 26.04) build container')
    arch = run(['dpkg', '--print-architecture']).stdout.strip()
    full = version.full()
    pkg_version = package_version(full, os_name, release)
    codename = SUITES[release]
    call('apt-get', 'update', '-qq')
    with tempfile.TemporaryDirectory(prefix='drivelink-bloom-') as scratch:
        work = Path(scratch)
        env = rosdep_environment(work, distro, codename)
        env.update(DRIVELINK_VERSION_FULL=full, DEB_BUILD_PROFILES='nocheck',
                   DEB_BUILD_OPTIONS=f'parallel={jobs} nocheck noautodbgsym')
        deps = fetch_dependencies(work)
        built = []
        for logical, (relative, dependencies) in ROS_PACKAGES.items():
            name = debian_name(distro, logical)
            say(f'Building {name} {pkg_version} ({arch})')
            package = stage(work, relative)
            call('bloom-generate', 'rosdebian', '--ros-distro', distro, '--os-name', os_name,
                 '--os-version', codename, '--native', cwd=package, env=env)
            control = configure_debian(package, logical, distro, pkg_version, dependencies, deps)
            call('apt-get', '-y', '--no-install-recommends', 'satisfy',
                 control['Build-Depends'], env=env)
            build_deb(package, env)
            deb = package.parent / deb_name(name, pkg_version, arch)
            if not deb.is_file():
                raise Failure(f'expected {deb.name}, found {[p.name for p in package.parent.glob("*.deb")]}')
            metadata = read_deb(deb)
            if (metadata.package, metadata.version, metadata.arch) != (name, pkg_version, arch):
                raise Failure(f'{deb.name} has inconsistent package, version, or architecture metadata')
            if dependencies:
                installed_depends = {value.strip() for value in metadata.field('Depends').split(',')}
                for dependency in dependencies:
                    wanted = f'{debian_name(distro, dependency)} (= {pkg_version})'
                    if wanted not in installed_depends:
                        raise Failure(f'{deb.name} is missing Depends: {wanted}')
            built.append(deb)
            if logical in ('drivelink_protocol', 'drivelink'):
                call('dpkg', '-i', deb, env=env)

        out.mkdir(parents=True, exist_ok=True)
        for stale in out.glob(f'ros-{distro}-drivelink*.deb'):
            stale.unlink()
        for deb in built:
            shutil.copy2(deb, out)
        say(f'Built {len(built)} {distro} packages in {out}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/ros-debs')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    try:
        build_packages(args.out.resolve(), args.jobs)
    except (Failure, RepoError) as error:
        print(f'package_ros: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
