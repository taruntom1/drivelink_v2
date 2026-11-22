"""H3c: the .deb packages installed, upgraded, removed and purged in a clean container.

    sudo python3 host/test/installed_packages_smoke.py --debs build/deb

Run as root in a clean ubuntu:26.04 or ubuntu:24.04 container (CI's
linux-install job) with this release's packages in --debs: drivelink-cli,
drivelink-agent and, on 26.04, drivelink (the GUI). Never on a development
machine: it installs, purges and creates the drivelink user.

The container has no systemd as PID 1. A recording `systemctl` stub in
/usr/sbin and a /run/systemd/system directory stand in for it, so the
maintainer scripts' service calls are checked exactly; the unit and udev rules
are checked with `systemd-analyze verify` and `udevadm verify` at the end.
"Old" packages are this build relabelled with a lower version (`~old`).

Agent: a first install enables and starts the service and makes the user and
the token; a hand-started agent (as drivelink, the installed config) answers
an empty device list; an upgrade makes no service call except daemon-reload
and is-active (the safety rule: an upgrade never restarts the agent), prints
the restart notice and leaves the running agent serving; remove stops and
disables it and keeps the config, token and user; a reinstall starts it
again with the same token; purge deletes the config, token and user.
GUI (26.04): H2g's single `drivelink` package (GUI, CLI and vdev) upgrades to
the split drivelink + drivelink-cli, and the files change owner.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

# dpkg runs maintainer scripts with a fixed PATH without /usr/local; the
# real systemctl (installed only at the end) is /usr/bin/systemctl.
STUB = Path('/usr/sbin/systemctl')
RUN_SYSTEMD = Path('/run/systemd/system')
CALLS = Path('/tmp/drivelink-systemctl.log')
ACTIVE = Path('/tmp/drivelink-systemctl.active')
CONF = Path('/etc/drivelink')
STUB_TEXT = f"""#!/bin/sh
# Recording stand-in for systemctl (installed_packages_smoke.py).
echo "$*" >> {CALLS}
for arg in "$@"; do
    if [ "$arg" = is-active ]; then
        [ -e {ACTIVE} ] && exit 0
        exit 3
    fi
done
exit 0
"""
ENV = dict(os.environ, DEBIAN_FRONTEND='noninteractive', PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin')


class Fail(Exception):
    pass


def expect(condition, message, **context):
    if not condition:
        details = ''.join(f'\n  {key}: {value}' for key, value in context.items())
        raise Fail(message + details)


def sh(*argv, check=True, timeout=900):
    done = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=timeout, env=ENV)
    if check and done.returncode != 0:
        raise Fail(f'{" ".join(map(str, argv))} exited {done.returncode}\n{done.stdout[-2000:]}\n{done.stderr[-2000:]}')
    return done


def apt(*args):
    done = sh('apt-get', '-y', '--no-install-recommends', *args)
    return done.stdout + done.stderr


def calls():
    lines = CALLS.read_text(encoding='utf-8').splitlines() if CALLS.exists() else []
    CALLS.unlink(missing_ok=True)
    return lines


def field(deb, name):
    return sh('dpkg-deb', '--field', deb, name).stdout.strip()


def find(debs, package):
    found = [p for p in Path(debs).glob(f'{package}_*.deb')]
    expect(len(found) == 1, f'one {package} package in {debs}', found=found)
    return found[0].resolve()


def relabel(deb, work, suffix='~old', merge=None):
    """A copy of `deb` with Version + suffix (in control and the maintainer
    scripts). With `merge`, also H2g's layout: merge's programs moved in, no
    Depends on drivelink-cli, merge's Depends added."""
    root = work / f'{deb.stem}{suffix}'
    shutil.rmtree(root, ignore_errors=True)
    sh('dpkg-deb', '-R', deb, root)
    control = (root / 'DEBIAN/control').read_text(encoding='utf-8')
    old = re.search(r'^Version: (.+)$', control, re.M).group(1)
    control = control.replace(f'Version: {old}', f'Version: {old}{suffix}')
    if merge is not None:
        extra = work / 'merge'
        shutil.rmtree(extra, ignore_errors=True)
        sh('dpkg-deb', '-x', merge, extra)
        for program in (extra / 'usr/bin').iterdir():
            shutil.copy2(program, root / 'usr/bin' / program.name)
        depends = [d.strip() for d in re.search(r'^Depends: (.+)$', control, re.M).group(1).split(',')]
        depends = [d for d in depends if not d.startswith('drivelink-cli')]
        depends += [d.strip() for d in field(merge, 'Depends').split(',') if d.strip() not in depends]
        control = re.sub(r'^Depends: .+$', 'Depends: ' + ', '.join(depends), control, flags=re.M)
    (root / 'DEBIAN/control').write_text(control, encoding='utf-8')
    for script in ('postinst', 'prerm', 'postrm'):
        path = root / 'DEBIAN' / script
        if path.exists():
            path.write_text(path.read_text(encoding='utf-8').replace(old, old + suffix), encoding='utf-8')
    out = work / f'{deb.stem}{suffix}.deb'
    sh('dpkg-deb', '--root-owner-group', '-b', root, out)
    return out


def installed_version(package):
    done = sh('dpkg-query', '-W', '-f=${Version} ${db:Status-Status}', package, check=False)
    return done.stdout.strip() if done.returncode == 0 else ''


def owner(path):
    return sh('dpkg', '-S', path).stdout.split(':', 1)[0]


def token():
    return (CONF / 'token').read_text(encoding='utf-8').strip()


class HandAgent:
    """The installed agent started by hand as drivelink with the installed config."""

    def __init__(self):
        self.process = subprocess.Popen(
            # setpriv execs the agent itself (runuser would stay between and die by the signal).
            ['setpriv', '--reuid=drivelink', '--regid=drivelink', '--init-groups',
             '/usr/bin/drivelink-agent', '--config', str(CONF / 'agent.json'),
             '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=ENV)
        seen = []
        for _ in range(20):  # its log lines (stderr) come first
            line = self.process.stdout.readline().strip()
            seen.append(line)
            if line.startswith('READY ') or not line:
                break
        expect(line.startswith('READY tcp://127.0.0.1:'), 'the hand-started agent is READY', lines=seen)
        self.url = line.split()[1]

    def devices(self):
        done = sh('/usr/bin/drivelink-cli', '--endpoint', self.url, '--token', token(), '--json', 'agent-devices')
        return json.loads(done.stdout.strip().splitlines()[-1])['data']['devices']

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(20)
        return self.process.returncode


def stub_on():
    STUB.write_text(STUB_TEXT, encoding='utf-8')
    STUB.chmod(0o755)
    RUN_SYSTEMD.mkdir(parents=True, exist_ok=True)
    CALLS.unlink(missing_ok=True)
    ACTIVE.unlink(missing_ok=True)


def stub_off():
    STUB.unlink(missing_ok=True)
    if RUN_SYSTEMD.is_dir():
        RUN_SYSTEMD.rmdir()
    CALLS.unlink(missing_ok=True)
    ACTIVE.unlink(missing_ok=True)


def service_calls(lines):
    """The calls that change the service (daemon-reload and is-active do not)."""
    return [line for line in lines if re.search(r'\b(start|stop|restart|try-restart|reload|enable|disable|kill)\b', line)
            and 'daemon-reload' not in line]


def agent_flow(cli_new, agent_new, work):
    cli_old, agent_old = relabel(cli_new, work), relabel(agent_new, work)
    stub_on()

    # First install.
    output = apt('install', cli_old, agent_old)
    expect(installed_version('drivelink-agent').endswith('~old installed'), 'the old agent is installed')
    lines = calls()
    expect('enable drivelink-agent.service' in lines and 'start drivelink-agent.service' in lines,
           'a first install enables and starts the service', calls=lines)
    expect('token is in /etc/drivelink/token' in output, 'the first-install notice', output=output[-1500:])
    groups = sh('id', '-nG', 'drivelink').stdout.split()
    expect('drivelink' in groups and 'dialout' in groups, 'the drivelink user, in dialout', groups=groups)
    expect(sh('getent', 'passwd', 'drivelink').stdout.strip().endswith(':/nonexistent:/usr/sbin/nologin'),
           'drivelink is a system user without a login')
    stat = os.stat(CONF / 'token')
    expect((stat.st_mode & 0o777, stat.st_uid) == (0o640, 0), 'the token is 0640 root', mode=oct(stat.st_mode))
    expect(stat.st_gid == int(sh('getent', 'group', 'drivelink').stdout.split(':')[2]), 'the token group is drivelink')
    first_token = token()
    expect(re.fullmatch(r'[0-9a-f]{64}', first_token), 'the token is 32 random bytes in hex')
    config = json.loads((CONF / 'agent.json').read_text(encoding='utf-8'))
    expect(config == {'bind': '127.0.0.1', 'port': 47620, 'token_file': 'token', 'devices': []}, 'the shipped config',
           config=config)
    conffiles = sh('dpkg-query', '-W', '-f=${Conffiles}', 'drivelink-agent').stdout
    expect('/etc/drivelink/agent.json' in conffiles, 'agent.json is a conffile', conffiles=conffiles)
    for path in ('/usr/lib/systemd/system/drivelink-agent.service', '/usr/lib/udev/rules.d/70-drivelink.rules',
                 '/usr/share/drivelink-agent/THIRD_PARTY_NOTICES.txt', '/usr/share/doc/drivelink-agent/copyright',
                 '/usr/share/drivelink-cli/THIRD_PARTY_NOTICES.txt', '/usr/bin/drivelink-vdev'):
        expect(Path(path).is_file(), f'{path} installed')
    print('PASS first install: service enabled and started, user, token (0640 root:drivelink), config', flush=True)

    # The agent as the service runs it, then an upgrade under it.
    agent = HandAgent()
    try:
        expect(agent.devices() == [], 'the installed agent lists no devices')
        ACTIVE.touch()
        output = apt('install', cli_new, agent_new)
        lines = calls()
        expect(service_calls(lines) == [], 'an upgrade makes no start/stop/restart/enable/disable call', calls=lines)
        expect('was NOT restarted' in output and 'systemctl restart drivelink-agent' in output,
               'the upgrade prints the restart notice', output=output[-1500:])
        expect(not installed_version('drivelink-agent').endswith('~old installed'), 'the new agent is installed')
        expect(agent.process.poll() is None and agent.devices() == [], 'the running agent still serves')
        expect(token() == first_token, 'the upgrade keeps the token')
    finally:
        code = agent.stop()
    expect(code == 0, 'the agent exits 0 on SIGTERM', code=code)
    print('PASS upgrade: no service restart, notice printed, running agent kept serving, token kept', flush=True)

    # Remove keeps the config, token and user; a reinstall starts the service again.
    ACTIVE.unlink(missing_ok=True)
    apt('remove', 'drivelink-agent')
    lines = calls()
    expect('stop drivelink-agent.service' in lines and 'disable drivelink-agent.service' in lines,
           'remove stops and disables the service', calls=lines)
    expect(not Path('/usr/bin/drivelink-agent').exists(), 'remove deletes the program')
    expect((CONF / 'agent.json').is_file() and token() == first_token, 'remove keeps the config and token')
    expect(sh('getent', 'passwd', 'drivelink', check=False).returncode == 0, 'remove keeps the user')
    apt('install', agent_new)
    lines = calls()
    expect('start drivelink-agent.service' in lines, 'a reinstall after remove starts the service', calls=lines)
    expect(token() == first_token, 'a reinstall keeps the token')
    print('PASS remove keeps config, token and user; reinstall starts the service again', flush=True)

    # The unit and rules with the real tools (systemd is not PID 1 here).
    stub_off()
    apt('install', 'systemd', 'udev')
    done = sh('systemd-analyze', 'verify', '/usr/lib/systemd/system/drivelink-agent.service', check=False)
    expect(done.returncode == 0 and not done.stderr.strip(), 'systemd-analyze verify is clean',
           stdout=done.stdout, stderr=done.stderr)
    done = sh('udevadm', 'verify', '/usr/lib/udev/rules.d/70-drivelink.rules', check=False)
    expect(done.returncode == 0, 'udevadm verify accepts the rules', stdout=done.stdout, stderr=done.stderr)
    done = sh('/usr/bin/drivelink-agent', '--list-ports', check=False)
    expect(done.returncode in (0, 1), 'drivelink-agent --list-ports runs', code=done.returncode, stderr=done.stderr)
    print('PASS systemd-analyze verify and udevadm verify', flush=True)

    apt('purge', 'drivelink-agent', 'drivelink-cli')
    left = [p for p in ('/etc/drivelink', '/var/lib/drivelink-agent', '/usr/bin/drivelink-agent',
                        '/usr/bin/drivelink-cli', '/usr/share/drivelink-agent') if Path(p).exists()]
    expect(not left, 'purge leaves nothing behind', left=left)
    expect(sh('getent', 'passwd', 'drivelink', check=False).returncode != 0, 'purge deletes the user')
    expect(sh('getent', 'group', 'drivelink', check=False).returncode != 0, 'purge deletes the group')
    print('PASS purge: config, token, user and group gone', flush=True)


def gui_flow(cli_new, gui_new, work):
    h2g = relabel(gui_new, work, merge=cli_new)
    apt('install', h2g)
    expect(owner('/usr/bin/drivelink-cli') == 'drivelink', "H2g's package owns the CLI")
    apt('install', gui_new, cli_new)
    for path, package in (('/usr/bin/drivelink-cli', 'drivelink-cli'), ('/usr/bin/drivelink-vdev', 'drivelink-cli'),
                          ('/usr/bin/drivelink-gui', 'drivelink')):
        expect(owner(path) == package, f'{path} belongs to {package} after the upgrade', owner=owner(path))
    expect(not installed_version('drivelink').endswith('~old installed'), 'the split GUI package is installed')
    expect(sh('/usr/bin/drivelink-cli', '--help', check=False).returncode == 0, 'the CLI runs after the upgrade')
    apt('purge', 'drivelink', 'drivelink-cli')
    left = [p for p in ('/usr/bin/drivelink-gui', '/usr/bin/drivelink-cli', '/usr/share/drivelink',
                        '/usr/share/drivelink-cli') if Path(p).exists()]
    expect(not left, 'purge leaves nothing behind', left=left)
    print("PASS H2g's drivelink upgrades to drivelink + drivelink-cli; files change owner", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--debs', required=True, type=Path, help="this release's packages")
    args = parser.parse_args()
    if os.geteuid() != 0:
        print('installed_packages_smoke: run as root in a clean container', file=sys.stderr)
        return 2
    started = time.monotonic()
    cli_new, agent_new = find(args.debs, 'drivelink-cli'), find(args.debs, 'drivelink-agent')
    guis = list(args.debs.glob('drivelink_*.deb'))
    sh('apt-get', 'update')
    with tempfile.TemporaryDirectory(prefix='drivelink-packages-') as work:
        try:
            agent_flow(cli_new, agent_new, Path(work))
            if guis:
                gui_flow(cli_new, find(args.debs, 'drivelink'), Path(work))
            else:
                print('skipped: no GUI package for this release', flush=True)
        except Fail as failure:
            print(f'FAIL {failure}', file=sys.stderr)
            return 1
        finally:
            stub_off()
    print(f'PASS installed packages ({time.monotonic() - started:.0f} s)', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
