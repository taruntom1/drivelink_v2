"""H3c: the agent packages on a real host with systemd (WSL or the Pi), with a device.

    sudo python3 host/test/deploy_check.py --debs DIR --phase install [--endpoint auto|vdev|ENDPOINT] [--bind ADDR]
    sudo python3 host/test/deploy_check.py --debs DIR --phase after-reboot
    sudo python3 host/test/deploy_check.py --debs DIR --phase remove
    sudo python3 host/test/deploy_check.py --debs DIR --phase all     # install, then remove (the WSL run)

Run as root on a host whose PID 1 is systemd and where no drivelink-agent is
installed. DIR holds this release's drivelink-cli and drivelink-agent .debs.

install: installs a relabelled lower version (`~old`) of both packages; the
service must be active and log "no devices configured". It adds one device
to /etc/drivelink/agent.json: `auto` takes the first line `drivelink-agent
--list-ports` prints (a /dev/drivelink/<serial> name from the udev rule, or
/dev/serial0), `vdev` starts the installed drivelink-vdev and lists it (the
mock mode: no hardware; with --phase all only), or an explicit endpoint. After `systemctl restart`
the device must come online through the agent and answer `info` and
`status` (read only; disarmed). Then it upgrades to DIR's packages: the
service's MainPID must not change (an upgrade never restarts the agent) and
the device stays online; `systemctl restart` then loads the new version.
--bind ADDR (a LAN address) sets the bind before the restart, so the agent
advertises itself for a GUI on another machine; the token is printed.
after-reboot: the service is enabled and active and the device online again.
remove: `apt remove` stops the service and keeps the config and token;
`apt purge` deletes them and the drivelink user. Exit 0 pass, 1 fail, 2 refused.

The agent DISARMs the device when it opens it and when it stops; this script
never arms, writes or reboots the device.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from installed_packages_smoke import ENV, Fail, expect, find, relabel, sh  # noqa: E402

CONF = Path('/etc/drivelink')
UNIT = 'drivelink-agent.service'
ALIAS = 'check'
URL = 'tcp://127.0.0.1:47620'
VDEV = []  # the mock device, stopped at the end


def apt(*args):
    done = sh('apt-get', '-y', '--no-install-recommends', *args)
    return done.stdout + done.stderr


def token():
    return (CONF / 'token').read_text(encoding='utf-8').strip()


def cli(*args, device=True, code=0):
    argv = ['/usr/bin/drivelink-cli', '--endpoint', URL, '--token', token()]
    if device:
        argv += ['--device', ALIAS]
    done = sh(*argv, '--json', *args, check=False, timeout=60)
    expect(done.returncode == code, f'drivelink-cli {" ".join(args)} exited {done.returncode}',
           stdout=done.stdout[-800:], stderr=done.stderr[-800:])
    return json.loads(done.stdout.strip().splitlines()[-1])


def main_pid():
    return int(sh('systemctl', 'show', '-p', 'MainPID', '--value', UNIT).stdout.strip() or 0)


def wait_active(timeout=20):
    deadline = time.monotonic() + timeout
    while sh('systemctl', 'is-active', '--quiet', UNIT, check=False).returncode != 0:
        expect(time.monotonic() < deadline, 'the service is active',
               status=sh('systemctl', 'status', '--no-pager', UNIT, check=False).stdout[-1500:])
        time.sleep(0.5)
    return main_pid()


def wait_online(timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        devices = []
        if sh('systemctl', 'is-active', '--quiet', UNIT, check=False).returncode == 0:
            try:
                devices = cli('agent-devices', device=False)['data']['devices']
            except (Fail, ValueError, KeyError):
                devices = []
        if devices and devices[0]['alias'] == ALIAS and devices[0]['online']:
            return devices[0]
        expect(time.monotonic() < deadline, f'{ALIAS} comes online through the agent', devices=devices,
               journal=journal()[-1500:])
        time.sleep(0.5)


def journal(since=None):
    argv = ['journalctl', '-u', UNIT, '--no-pager', '-o', 'cat']
    if since:
        argv += ['--since', since]
    return sh(*argv, check=False).stdout


def check_device():
    record = wait_online()
    info = cli('info')['data']
    status = cli('status')['data']
    expect(status.get('arm_state', 0) == 0, 'the device is disarmed', status=status)
    print(f'PASS {record["endpoint"]} online through the agent: device {info.get("device_id")} '
          f'firmware {info["firmware_version"]}, disarmed', flush=True)


def device_line(endpoint, work):
    if endpoint == 'vdev':
        vdev = subprocess.Popen(['/usr/bin/drivelink-vdev', '--port', '0', '--state', str(work / 'vdev')],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=ENV)
        VDEV.append(vdev)
        line = vdev.stdout.readline().strip()
        expect(line.startswith('READY '), 'the installed vdev is READY', line=line)
        print(f'mock mode: drivelink-vdev at {line.split()[1]}', flush=True)
        return {'alias': ALIAS, 'endpoint': line.split()[1]}
    if endpoint == 'auto':
        done = sh('/usr/bin/drivelink-agent', '--list-ports', check=False)
        print(done.stdout.rstrip(), flush=True)
        lines = [json.loads(line) for line in done.stdout.splitlines() if line.startswith('{')]
        expect(lines, 'drivelink-agent --list-ports found a port', stderr=done.stderr)
        return dict(lines[0], alias=ALIAS)
    return {'alias': ALIAS, 'endpoint': endpoint, 'baud': 921600}


def write_config(device, bind=None):
    config = json.loads((CONF / 'agent.json').read_text(encoding='utf-8'))
    config['devices'] = [device]
    if bind:
        config['bind'] = bind
    (CONF / 'agent.json').write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')


def phase_install(args, work):
    cli_new, agent_new = find(args.debs, 'drivelink-cli'), find(args.debs, 'drivelink-agent')
    started = time.strftime('%Y-%m-%d %H:%M:%S')
    output = apt('install', relabel(cli_new, work), relabel(agent_new, work))
    expect('token is in /etc/drivelink/token' in output, 'the first-install notice', output=output[-1500:])
    first = wait_active()
    expect(sh('systemctl', 'is-enabled', UNIT).stdout.strip() == 'enabled', 'the service is enabled')
    time.sleep(1)
    expect('no devices configured' in journal(started), 'the agent logs that no devices are configured',
           journal=journal(started)[-1500:])
    expect(cli('agent-devices', device=False)['data']['devices'] == [], 'the lobby lists no devices')
    print(f'PASS installed {cli_new.name.split("_")[1]}~old: service active (pid {first}), no devices', flush=True)

    device = device_line(args.endpoint, work)
    write_config(device, args.bind)
    sh('systemctl', 'restart', UNIT)
    before = wait_active()
    expect(before != first, 'systemctl restart started a new agent')
    check_device()

    output = apt('install', cli_new, agent_new)
    expect('was NOT restarted' in output, 'the upgrade prints the restart notice', output=output[-1500:])
    expect(main_pid() == before, 'the upgrade did not restart the agent', before=before, after=main_pid())
    expect(wait_online()['online'], 'the device stayed online through the upgrade')
    print(f'PASS upgrade: MainPID {before} unchanged, device online, notice printed', flush=True)

    sh('systemctl', 'restart', UNIT)
    after = wait_active()
    expect(after != before, 'the restart loaded the new agent')
    check_device()
    if args.bind:
        print(f'agent bound to {args.bind}:47620, advertising; token: {token()}', flush=True)
    print('PASS install phase', flush=True)


def phase_after_reboot(args, work):
    expect(sh('systemctl', 'is-enabled', UNIT).stdout.strip() == 'enabled', 'the service is enabled')
    pid = wait_active(60)
    check_device()
    print(f'PASS after reboot: service active (pid {pid}), device online', flush=True)


def phase_remove(args, work):
    kept = token()
    apt('remove', 'drivelink-agent')
    expect(sh('systemctl', 'is-active', '--quiet', UNIT, check=False).returncode != 0, 'remove stops the service')
    expect((CONF / 'agent.json').is_file() and token() == kept, 'remove keeps the config and token')
    apt('purge', 'drivelink-agent', 'drivelink-cli')
    expect(not CONF.exists(), 'purge deletes /etc/drivelink')
    expect(sh('getent', 'passwd', 'drivelink', check=False).returncode != 0, 'purge deletes the drivelink user')
    print('PASS remove keeps the config and token; purge deletes them and the user', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--debs', required=True, type=Path)
    parser.add_argument('--phase', required=True, choices=['install', 'after-reboot', 'remove', 'all'])
    parser.add_argument('--endpoint', default='auto', help='auto (--list-ports), vdev (mock), or an endpoint')
    parser.add_argument('--bind', help='a LAN address for the agent (advertises over mDNS)')
    args = parser.parse_args()
    if args.endpoint == 'vdev' and args.phase != 'all':
        print('deploy_check: --endpoint vdev (the mock device) needs --phase all', file=sys.stderr)
        return 2
    if os.geteuid() != 0 or not Path('/run/systemd/system').is_dir():
        print('deploy_check: run as root on a host whose PID 1 is systemd', file=sys.stderr)
        return 2
    installed = sh('dpkg-query', '-W', '-f=${db:Status-Status}', 'drivelink-agent', check=False).stdout.strip()
    if args.phase in ('install', 'all') and installed not in ('', 'not-installed'):
        print(f'deploy_check: drivelink-agent is already {installed}; purge it first', file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix='drivelink-deploy-') as work:
        try:
            phases = {'install': [phase_install], 'after-reboot': [phase_after_reboot],
                      'remove': [phase_remove], 'all': [phase_install, phase_remove]}[args.phase]
            for phase in phases:
                phase(args, Path(work))
        except Fail as failure:
            print(f'FAIL {failure}', file=sys.stderr)
            return 1
        finally:
            for vdev in VDEV:
                vdev.kill()
                vdev.wait(10)
    print('PASS deploy check', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
