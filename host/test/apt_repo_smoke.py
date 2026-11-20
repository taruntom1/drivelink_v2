"""H3d: DriveLink's signed apt repository, used the way a user does.

    sudo python3 host/test/apt_repo_smoke.py --repo build/apt --key build/apt-key.asc
    sudo python3 host/test/apt_repo_smoke.py --url https://<host>/<path> [--key-url URL] --phase install|upgrade

Run as root in a clean ubuntu:26.04 or ubuntu:24.04 container (CI's apt-install
job, or locally in Docker), or on the Pi for the --url rows. Never on a
development machine: it adds an apt source and key, installs and purges
packages and creates the drivelink user. Ubuntu's own sources stay: the
packages' dependencies come from them.

--repo serves a built repository (apt_repo.py build) on 127.0.0.1 and checks:
the key and a deb822 source are installed and `apt-get update` is clean for
the DriveLink URL; the candidates of drivelink-agent and drivelink-cli come
from it with this release's `~ubuntuNN.NN` version; they install (systemctl is
a recording stub as in installed_packages_smoke.py, whose helpers this reuses;
its calls are printed, not asserted); on 26.04 the GUI resolves; a copy signed
by another key is refused (NO_PUBKEY / not signed) and never becomes a
candidate; the packages purge.

--url (the deferred Pi rows) installs from the published repository over
HTTPS with the real systemd. --phase install does the first install and checks
the service is active; --phase upgrade, run after a newer release is published,
upgrades and checks that the agent was not restarted (same MainPID) and that
the restart notice was printed.
"""
import argparse
import hashlib
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from installed_packages_smoke import (Fail, expect, sh, apt, stub_on, stub_off, calls, service_calls,  # noqa: E402
                                      installed_version)

KEYRING_NAME = 'drivelink-archive-keyring.asc'
KEY = Path('/etc/apt/keyrings') / KEYRING_NAME
SOURCES = Path('/etc/apt/sources.list.d/drivelink.sources')
PACKAGES = ('drivelink-agent', 'drivelink-cli')
PROBLEM = ('W:', 'E:', 'Err:')
REFUSED = ('no_pubkey', 'not signed', 'signature', 'invalid')


def passed(text):
    print(f'PASS {text}', flush=True)


class Quiet(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    """Serve `directory` on a free loopback port; returns (server, url)."""
    server = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(directory)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f'http://127.0.0.1:{server.server_address[1]}'


def os_release():
    values = {}
    for line in Path('/etc/os-release').read_text(encoding='utf-8').splitlines():
        name, found, value = line.partition('=')
        if found:
            values[name] = value.strip('"')
    return values['VERSION_CODENAME'], values['VERSION_ID']


def sources_text(url, codename, key=KEY):
    return f'Types: deb\nURIs: {url}/\nSuites: {codename}\nComponents: main\nSigned-By: {key}\n'


def install_source(url, codename, key_bytes):
    sh('install', '-d', '-m', '0755', KEY.parent)
    KEY.write_bytes(key_bytes)
    KEY.chmod(0o644)
    SOURCES.write_text(sources_text(url, codename), encoding='utf-8')


def update(url):
    """`apt-get update` (the system sources too); the problem lines that name `url`.
    Its exit code varies with apt's version and the failure, so only the output is judged."""
    done = sh('apt-get', 'update', check=False)
    output = done.stdout + done.stderr
    return output, [line for line in output.splitlines() if line.startswith(PROBLEM) and url in line]


def policy(*packages):
    """{package: (candidate version, the package's whole policy block)}"""
    blocks, name = {}, None
    for line in sh('apt-cache', 'policy', *packages).stdout.splitlines():
        if line and not line[0].isspace() and line.endswith(':'):
            name = line[:-1]
            blocks[name] = []
        elif name:
            blocks[name].append(line)
    result = {}
    for name, lines in blocks.items():
        text = '\n'.join(lines)
        found = re.search(r'Candidate: (\S+)', text)
        result[name] = (found.group(1) if found else '(none)', text)
    return result


def show_key(key, url):
    print(f'the repository key: sha256 {hashlib.sha256(key).hexdigest()}', flush=True)
    if not shutil.which('gpg'):
        print(f'gpg is not installed: compare the fingerprint shown on {url}/index.html with the key you expect',
              flush=True)
        return
    with tempfile.TemporaryDirectory(prefix='dlapt-show-') as home:
        (Path(home) / 'key.asc').write_bytes(key)
        print(sh('gpg', '--homedir', home, '--batch', '--no-tty', '--show-keys', '--fingerprint',
                 Path(home) / 'key.asc').stdout, flush=True)


def check_candidates(url, release):
    found = policy(*PACKAGES)
    for package in PACKAGES:
        version, block = found.get(package, ('(none)', ''))
        expect(version.endswith(f'~ubuntu{release}'), f'the {package} candidate is this release\'s build',
               candidate=version, release=release)
        expect(url in block, f'the {package} candidate comes from the DriveLink repository', policy=block)
    return {package: found[package][0] for package in PACKAGES}


def install_flow(url, key_bytes, codename, release, stubbed):
    install_source(url, codename, key_bytes)
    passed(f'key and source installed ({SOURCES.name} for {codename})')

    output, bad = update(url)
    expect(not bad, 'apt-get update has no warning or error for the DriveLink repository', lines=bad,
           output=output[-1500:])
    passed('apt-get update: the repository is read and its signature accepted')

    wanted = check_candidates(url, release)
    passed(f'apt-cache policy: {wanted["drivelink-agent"]} comes from the repository')

    if stubbed:
        stub_on()
    try:
        apt('install', *PACKAGES)
        if stubbed:
            lines = calls()
            print(f'systemctl calls during the first install: {lines}', flush=True)
            print(f'  of which change the service: {service_calls(lines)}', flush=True)
    finally:
        if stubbed:
            stub_off()
    for package in PACKAGES:
        expect(installed_version(package) == f'{wanted[package]} installed',
               f'{package} is installed at the candidate version', installed=installed_version(package),
               candidate=wanted[package])
    if not stubbed:
        done = sh('systemctl', 'is-active', 'drivelink-agent', check=False)
        expect(done.stdout.strip() == 'active', 'the drivelink-agent service is active', stdout=done.stdout)
    passed('drivelink-agent and drivelink-cli installed from the repository')

    if release == '26.04':
        sh('apt-get', 'install', '-s', 'drivelink')
        passed('the GUI package (drivelink) resolves')
    else:
        print('skipped: no GUI package for this release', flush=True)


def refusal(repo, key_bytes, codename):
    """A copy of the repository signed by another key is refused: apt names it and it offers nothing."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
    from apt_repo import Gpg  # lazily: --url mode never needs it (nor gpg)
    with tempfile.TemporaryDirectory(prefix='dlapt-bad-') as tmp:
        bad_repo = Path(tmp) / 'repo'
        shutil.copytree(repo, bad_repo)
        release = bad_repo / 'dists' / codename / 'Release'
        for signature in ('InRelease', 'Release.gpg'):
            (release.parent / signature).unlink()
        with Gpg() as gpg:
            gpg.sign(gpg.generate('DriveLink other key'), release)
        expect((release.parent / 'InRelease').is_file() and (release.parent / 'Release.gpg').is_file(),
               'the copy is signed again', files=sorted(p.name for p in release.parent.iterdir()))

        server, bad_url = serve(bad_repo)
        good = Path(tmp) / 'good.sources'
        other = SOURCES.with_name('drivelink-bad.sources')
        shutil.move(SOURCES, good)
        try:
            other.write_text(sources_text(bad_url, codename), encoding='utf-8')
            output, lines = update(bad_url)
            expect(lines and any(phrase in line.lower() for line in lines for phrase in REFUSED),
                   'apt-get update refuses the repository signed by another key', lines=lines,
                   output=output[-1500:])
            offered = sh('apt-cache', 'policy', *PACKAGES).stdout
            expect(bad_url not in offered, 'the refused repository offers no package', policy=offered)
        finally:
            server.shutdown()
            other.unlink(missing_ok=True)
            shutil.move(good, SOURCES)
        for line in lines:
            print(f'  apt: {line}', flush=True)
    passed('a repository signed by another key is refused and offers no package')


def repo_mode(args):
    codename, release = os_release()
    key_bytes = args.key.read_bytes()
    server, url = serve(args.repo)
    try:
        install_flow(url, key_bytes, codename, release, stubbed=True)
        refusal(args.repo, key_bytes, codename)
        stub_on()
        try:
            apt('purge', *PACKAGES)
        finally:
            stub_off()
        for package in PACKAGES:
            expect(not installed_version(package).endswith(' installed'), f'{package} is purged',
                   installed=installed_version(package))
        passed('drivelink-agent and drivelink-cli purged')
    finally:
        server.shutdown()


def url_mode(args):
    url = args.url.rstrip('/')
    if args.phase == 'upgrade':
        return upgrade_flow()
    key_url = args.key_url or f'{url}/{KEYRING_NAME}'
    with urllib.request.urlopen(key_url, timeout=60) as response:
        key_bytes = response.read()
    show_key(key_bytes, url)
    codename, release = os_release()
    # The real systemd runs on the Pi: no stub (stub_off would unlink /usr/sbin/systemctl).
    install_flow(url, key_bytes, codename, release, stubbed=False)


def main_pid():
    return sh('systemctl', 'show', '-p', 'MainPID', 'drivelink-agent').stdout.strip()


def upgrade_flow():
    before = {package: installed_version(package) for package in PACKAGES}
    expect(all(v.endswith(' installed') for v in before.values()),
           'both packages are installed (run --phase install first)', installed=before)
    pid = main_pid()
    expect(pid not in ('MainPID=0', 'MainPID='), 'the agent is running', pid=pid)
    sh('apt-get', 'update')
    output = apt('install', '--only-upgrade', *PACKAGES)
    after = {package: installed_version(package) for package in PACKAGES}
    for package in PACKAGES:
        old, new = before[package].split()[0], after[package].split()[0]
        print(f'{package}: {old} -> {new}', flush=True)
        expect(old != new and sh('dpkg', '--compare-versions', new, 'gt', old, check=False).returncode == 0,
               'no newer version published', package=package, old=old, new=new)
    expect(main_pid() == pid, 'the upgrade did not restart the agent (same MainPID)', before=pid, after=main_pid())
    expect('systemctl restart drivelink-agent' in output, 'the upgrade prints the restart notice',
           output=output[-1500:])
    passed('upgrade: newer version installed, agent not restarted, restart notice printed')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repo', type=Path, help='a repository built by tools/apt_repo.py (served on 127.0.0.1)')
    parser.add_argument('--key', type=Path, help="with --repo: the repository's public key file")
    parser.add_argument('--url', help='a published repository (the Pi rows)')
    parser.add_argument('--key-url', help=f'with --url: the public key (default <url>/{KEYRING_NAME})')
    parser.add_argument('--phase', choices=('install', 'upgrade'), help='with --url')
    args = parser.parse_args()
    if bool(args.repo) == bool(args.url):
        parser.error('give exactly one of --repo and --url')
    if args.repo and not args.key:
        parser.error('--repo needs --key')
    if args.url and not args.phase:
        parser.error('--url needs --phase')
    if not hasattr(os, 'geteuid') or os.geteuid() != 0:
        print('apt_repo_smoke: run as root in a clean container (or on the Pi for --url)', file=sys.stderr)
        return 2
    started = time.monotonic()
    try:
        if args.repo:
            repo_mode(args)
        else:
            url_mode(args)
    except Fail as failure:
        print(f'FAIL {failure}', file=sys.stderr)
        return 1
    finally:
        if args.repo:
            stub_off()
    print(f'PASS apt repository ({time.monotonic() - started:.0f} s)', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
