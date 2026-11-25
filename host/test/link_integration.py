"""Link trial and baud auto-detection of libdrivelink against an independent real-core vdev.

A PTY carries the rate and the wires in vdev, so the trials there are real: the
device cannot be reached at another rate. TCP runs the same trials without them
(the only transport on Windows).
"""
import argparse
import json
import queue
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

RATES = (115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000)
TRIAL_MS = 5000
TIMEOUT_KIND, REJECTED_KIND = 1, 3  # drivelink::ErrorKind


class Device:
    def __init__(self, args, root, pty):
        self.args, self.root, self.pty = args, root, pty
        self.process, self.lines = None, queue.Queue()
        self.scenario, self.sequence = Path(root) / 'scenario', 0

    def start(self):
        self.lines = queue.Queue()
        self.sequence = 0
        self.scenario.write_text('')
        self.process = subprocess.Popen(
            [self.args.vdev, '--state', f'{self.root}/state', '--link-trial-ms', str(TRIAL_MS), '--scenario-file',
             str(self.scenario), *(['--pty'] if self.pty else ['--port', '0'])],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        def pump(stream, lines):
            for line in stream:
                lines.put(line.strip())
        threading.Thread(target=pump, args=(self.process.stdout, self.lines), daemon=True).start()
        line = self.lines.get(timeout=15)
        assert line.startswith('READY '), line
        self.endpoint = line[6:]

    def kill(self):
        if self.process is not None:
            self.process.kill()
            self.process.wait(timeout=10)
            self.process.stdout.close()
            self.process = None

    def inject(self, action):
        self.sequence += 1
        replacement = self.scenario.with_suffix('.new')
        replacement.write_text(f'{self.sequence} {action}\n')
        replacement.replace(self.scenario)
        expected = f'SCENARIO {self.sequence} {action.split()[0]}'
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            line = self.lines.get(timeout=5)
            if line == expected:
                return
        raise AssertionError(f'no {expected}')

    def confirms(self):
        self.inject('stats')
        stats = dict(line.split(' ', 1) for line in Path(f'{self.scenario}.stats').read_text().splitlines()
                     if not line.startswith('group'))
        return int(stats['confirms'])

    def probe(self, *command, ok=True, timeout=60):
        result = subprocess.run([self.args.probe, self.endpoint, *map(str, command)], capture_output=True, text=True,
                                timeout=timeout)
        assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
        rows = [line.split('\t') for line in result.stdout.splitlines()]
        return Output(rows)

    def state(self):
        return self.probe('--baud', 'auto', 'state')

    def cli(self, *command, ok=True, json_mode=True, timeout=30):
        argv = [self.args.cli, '--endpoint', self.endpoint, *(['--json'] if json_mode else []), *map(str, command)]
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
        if not json_mode:
            return result.stdout
        return json.loads(result.stdout.strip().splitlines()[-1])


def check_cli(device):
    """CLI --baud auto (default), link-confirm, info link_trial_ms, and reboot trial hint."""
    # Default --baud is auto: connects cleanly without --baud and shows link_trial_ms == 0.
    info = device.cli('info')['data']
    assert info['link_trial_ms'] == 0, info
    # No trial running: link-confirm reports no_trial without sending LINK_CONFIRM.
    before = device.confirms()
    none = device.cli('link-confirm')['data']
    assert none['confirmed'] is False and none['outcome'] == 'no_trial', none
    assert device.confirms() == before

    # Stage 460800 (enum 2), save, reboot (readable output prints pending trial + confirm command).
    device.cli('set', '0:0:48=2')
    device.cli('save')
    text = device.cli('reboot', json_mode=False)
    assert 'link-confirm' in text and '460800' in text, text
    # Auto-detect finds 460800 in trial and link-confirm confirms it.
    deadline = time.monotonic() + 8
    while True:
        try:
            trial_info = device.cli('info')['data']
            if trial_info['link_trial_ms'] > 0:
                break
        except AssertionError:
            pass
        assert time.monotonic() < deadline, 'CLI did not see link trial after reboot'
        time.sleep(0.2)
    confirmed = device.cli('link-confirm')['data']
    assert confirmed['confirmed'] is True and confirmed['outcome'] == 'confirmed', confirmed
    assert confirmed['running']['baud'] == 460800 and confirmed['link_trial_ms'] == 0, confirmed
    if device.pty:
        assert confirmed['baud'] == 460800, confirmed
        # Explicit --baud auto and fixed --baud 460800 both work; wrong fixed baud times out.
        assert device.cli('--baud', 'auto', 'info')['data']['link_trial_ms'] == 0
        assert device.cli('--baud', '460800', 'info')['data']['link_trial_ms'] == 0
        device.cli('--baud', '115200', '--timeout-ms', '300', 'info', ok=False)

    # Unconfirmed trial reverts back to 460800.
    device.cli('set', '0:0:48=0')
    device.cli('save')
    reboot = device.cli('reboot')['data']
    assert reboot['link_trial_pending'] is True and reboot['trial_baud'] == 115200, reboot
    assert 'link-confirm' in reboot['confirm_command'], reboot
    deadline = time.monotonic() + 8
    while True:
        try:
            trial_info = device.cli('info')['data']
            if trial_info['link_trial_ms'] > 0:
                break
        except AssertionError:
            pass
        assert time.monotonic() < deadline, 'CLI did not see unconfirmed trial'
        time.sleep(0.2)
    # Wait for the trial (TRIAL_MS) to revert to 460800.
    deadline = time.monotonic() + TRIAL_MS / 1000 + 8
    while True:
        try:
            after = device.cli('info')['data']
            running = device.cli('get', '0:0:48')['data']['typed_values'][0]
            if after['link_trial_ms'] == 0 and running == 2:
                break
        except AssertionError:
            pass
        assert time.monotonic() < deadline, 'unconfirmed trial did not revert to 460800'
        time.sleep(0.3)

    # Restore 921600 (enum 3) and confirm.
    device.cli('set', '0:0:48=3')
    device.cli('save')
    device.cli('reboot')
    deadline = time.monotonic() + 8
    while True:
        try:
            res = device.cli('link-confirm')['data']
            if res['confirmed'] is True:
                break
        except AssertionError:
            pass
        assert time.monotonic() < deadline, 'CLI could not restore 921600'
        time.sleep(0.2)
    assert device.cli('get', '0:0:48')['data']['typed_values'][0] == 3


class Output(dict):
    """The last row of every key; `all` keeps them in order."""

    def __init__(self, rows):
        super().__init__({row[0]: row[1:] for row in rows})
        self.all = rows

    def text(self, key):
        return self[key][0]

    def outcomes(self):
        return [row[1] for row in self.all if row[0] == 'OUTCOME']


def trial(device, baud, pins=(43, 44), command='trial', options=()):
    return device.probe('--baud', 'auto', *options, command, baud, *pins)


def check_trials(device):
    # Confirmed: it runs, the next boot runs it, and no trial is left.
    out = trial(device, 115200)
    assert out.outcomes()[-1] == 'Confirmed', out.all
    assert out.text('RUNNING') == out.text('NEXT') == '115200 43 44' and out.text('TRIAL') == '0', out.all
    if device.pty:
        assert out.text('BAUD') == '115200', out.all
    # ... and across a restart of the device.
    device.kill()
    device.start()
    assert device.state().text('RUNNING') == '115200 43 44'

    # Not confirmed: seen in its trial, then back on the confirmed link.
    out = trial(device, 460800, command='watch')
    assert out.outcomes()[0] == 'Unconfirmed' and out.outcomes()[-1] == 'Reverted', out.all
    in_trial = next(row for row in out.all if row[0] == 'TRIAL' and row[1] != '0')
    assert 0 < int(in_trial[1]) <= TRIAL_MS, out.all
    assert out.text('RUNNING') == out.text('NEXT') == '115200 43 44', out.all
    if device.pty:
        assert out.text('BAUD') == '115200', out.all

    # A lost LINK_CONFIRM reply: settled by reading, sent once.
    before = device.confirms()
    device.inject('drop-confirm-reply 1')
    out = trial(device, 230400)
    assert out.outcomes()[-1] == 'Confirmed' and out.text('RUNNING') == '230400 43 44', out.all
    assert device.confirms() == before + 1

    # A lost LINK_CONFIRM: the reads show the trial still runs, so it goes out again.
    before = device.confirms()
    device.inject('drop-confirm 1')
    out = trial(device, 921600)
    assert out.outcomes()[-1] == 'Confirmed' and out.text('RUNNING') == '921600 43 44', out.all
    assert device.confirms() == before + 2

    # Nothing runs: nothing is sent.
    before = device.confirms()
    assert device.probe('--baud', 'auto', 'confirm').text('CONFIRM') == 'NoTrial'
    assert device.confirms() == before

    # A device that dies in its trial comes back on the confirmed link.
    device.probe('--baud', 'auto', 'start', 1000000, 43, 44)
    deadline = time.monotonic() + 4
    while device.state().text('TRIAL') == '0':
        assert time.monotonic() < deadline, 'the trial did not start'
    device.kill()
    device.start()
    out = device.state()
    assert out.text('RUNNING') == out.text('NEXT') == '921600 43 44' and out.text('TRIAL') == '0', out.all

    # A trial that somebody else confirms meanwhile.
    device.probe('--baud', 'auto', 'start', 1500000, 43, 44)
    deadline = time.monotonic() + 4
    while (out := device.probe('--baud', 'auto', 'confirm')).text('CONFIRM') != 'Confirmed':
        assert time.monotonic() < deadline, out.all
    assert device.state().text('NEXT') == '1500000 43 44'
    assert trial(device, 921600).outcomes()[-1] == 'Confirmed'

    # Refused pins change nothing.
    out = device.probe('--baud', 'auto', 'trial', 921600, 43, 43, ok=False)
    assert out['ERROR'][0] == str(REJECTED_KIND), out.all
    assert device.state().text('NEXT') == '921600 43 44'


def check_rates(device):
    """PTY only: the rate decides who can talk."""
    for rate in (*[r for r in RATES if r != 921600], 921600):
        out = trial(device, rate)
        assert out.outcomes()[-1] == 'Confirmed' and out.text('BAUD') == str(rate), out.all
        # No hint, the right hint and a wrong one; a fixed rate finds only its own.
        assert device.state().text('BAUD') == str(rate)
        started = time.monotonic()
        assert device.probe('--baud', 'auto', '--last-baud', rate, 'state').text('BAUD') == str(rate)
        hinted = time.monotonic() - started
        assert device.probe('--baud', 'auto', '--last-baud', 57600, 'state').text('BAUD') == str(rate)
        assert device.probe('--baud', rate, 'state').text('BAUD') == str(rate)
        wrong = 115200 if rate != 115200 else 921600
        out = device.probe('--baud', wrong, '--timeout-ms', 300, 'state', ok=False)
        assert out['ERROR'][0] == str(TIMEOUT_KIND), out.all
        print(f'rate {rate}: found, with the hint in {hinted * 1000:.0f} ms', flush=True)

    # The search is bounded.
    device.inject('stall')
    started = time.monotonic()
    out = device.probe('--baud', 'auto', '--detect-ms', 1000, 'state', ok=False)
    assert out['ERROR'][0] == str(TIMEOUT_KIND) and time.monotonic() - started < 4, out.all
    device.inject('resume')

    # A device that answers only after a while, as after a reset, is found.
    device.inject('stall')
    timer = threading.Timer(1.5, lambda: device.inject('resume'))
    timer.start()
    out = device.probe('--baud', 'auto', '--detect-ms', 8000, 'state')
    timer.join()
    assert out.text('BAUD') == '921600', out.all


def check_pins(device):
    """PTY only: the wires stay on 43/44, or follow."""
    out = trial(device, 921600, (17, 18))
    assert out.outcomes() == ['Reverted'] and out.text('RUNNING') == '921600 43 44', out.all

    moved = threading.Timer(2, lambda: device.inject('wires 17 18'))
    moved.start()
    out = trial(device, 115200, (17, 18))
    moved.join()
    assert out.outcomes() == ['Confirmed'] and out.text('RUNNING') == '115200 17 18', out.all

    moved = threading.Timer(2, lambda: device.inject('wires 43 44'))
    moved.start()
    out = trial(device, 921600, (43, 44))
    moved.join()
    assert out.outcomes() == ['Confirmed'], out.all

    # A window shorter than the trial: not reachable.
    out = trial(device, 921600, (17, 18), options=('--window-ms', 2000))
    assert out.outcomes() == ['NotReachable'], out.all
    time.sleep(TRIAL_MS / 1000)
    assert device.state().text('RUNNING') == '921600 43 44'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', required=True)
    ap.add_argument('--cli')
    ap.add_argument('--vdev', required=True)
    args = ap.parse_args()
    for pty in (False, True) if sys.platform.startswith('linux') else (False,):
        with tempfile.TemporaryDirectory(prefix='drivelink-link-') as root:
            device = Device(args, root, pty)
            try:
                device.start()
                check_trials(device)
                if args.cli:
                    check_cli(device)
                if pty:
                    check_rates(device)
                    check_pins(device)
            finally:
                device.kill()
        print('PASS', 'PTY' if pty else 'TCP', flush=True)


if __name__ == '__main__':
    main()
