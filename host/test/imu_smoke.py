"""IMU signals and persistent calibration through the descriptor-driven CLI.

Default: imu.mock on vdev or the bare board. --modules real requires a UART
endpoint and a BNO055 on --sda/--scl. --missing-sensor checks clean failure on
the bare board instead. Requires a clean loops-only layout and idle groups;
restores the original saved layout/settings. Real calibration quality and
movement accuracy must be checked with the attached sensor in the HW campaign.
"""
import argparse
import math
import struct
import sys
import time
from motor_smoke import Device, preflight


def run(cli_path, endpoint, modules='mock', missing_sensor=False, sda=8, scl=9, address=0x28, stack_wait=0, calibration_wait=0):
    cli = Device(cli_path, endpoint)
    info, original, refs, settings = preflight(cli)
    if modules == 'real' and info['firmware_version'].endswith('-vdev'):
        raise RuntimeError('vdev has no physical IMU bus; no configuration was changed')
    type_ = 48 if modules == 'real' else 114
    types = cli('types')
    if not any(t['type'] == type_ for t in types):
        raise RuntimeError(f'IMU type {type_} unavailable; no configuration was changed')
    if modules == 'real' and not endpoint.startswith('serial://'):
        raise RuntimeError('real modules require UART; no configuration was changed')
    if missing_sensor and modules != 'real':
        raise RuntimeError('--missing-sensor requires --modules real; no configuration was changed')
    sensor = next(loop for loop in info['loops'] if not loop['realtime'])
    items = {i['name']: i['id'] for i in cli('items', type_)}
    assert all(n in items for n in ('yaw', 'quaternion.w', 'valid', 'sample_age_us', 'calibration_write'))
    instance = None
    def ref(name):
        return f'{type_}:{instance}:{items[name]}'
    def action(name, *args, **kwargs):
        return cli('action', ref(name), *args, **kwargs)
    def stream_frame(names):
        cli('sub-clear', 0)
        cli('sub-config', 0, sensor['id'], max(1, sensor['rate_hz']//20), *(ref(n) for n in names))
        return cli('sub-stream', 0)
    def stream(names):
        return stream_frame(names)['typed_values']
    def wait_dirty(expected):
        # A new subscription epoch yields a device-time boundary after the
        # action/reboot. Discard queued status older than that sample.
        boundary = stream_frame(['actual_mode'])['timestamp_us']
        deadline = time.monotonic()+15
        last = None
        while time.monotonic() < deadline:
            last = cli('status')
            if last['timestamp_us'] >= boundary and bool(last['flags'] & 8) == expected:
                return
            time.sleep(.05)
        raise AssertionError(('dirty status did not reach expected state',expected,boundary,last))
    try:
        instance = cli('add', type_, 'imu_smoke', sensor['id'])['instance']
        if modules == 'real':
            cli('set', f'{ref("sda_pin")}={sda}', f'{ref("scl_pin")}={scl}', f'{ref("address")}={address}')
        cli.reboot()
        if missing_sensor:
            records = cli('instances')
            assert next(r for r in records if r['type'] == type_)['state'] != 0, records
            cli('cmd-config', 0, 0)
            assert cli('arm', code=5)['detail'] == 32
            entries = cli('diag-entries')
            assert any(e['source'] == type_ and e['code'] == 1 for e in entries), entries
            print('PASS real IMU missing sensor: failed instance, InitFailed and ARM refused')
            if stack_wait: time.sleep(stack_wait)
            return
        names = ['valid','sample_count','quaternion.w','gyro.z','gravity.z','temperature','sample_age_us']
        values = stream(names)
        assert values[0] and values[1] > 0 and all(math.isfinite(v) for v in values[2:]), values
        assert abs(values[2]) <= 1.01 and abs(values[4]) <= 20 and -40 <= values[5] <= 85, values
        assert values[6] < 1000000, values
        if modules == 'real':
            levels = ['calibration_status.'+n for n in ('sys','gyro','accel','mag')]
            deadline = time.monotonic()+calibration_wait
            print('Real BNO055: calibrate gyro at rest, then move through accel orientations and magnetometer motions; waiting for levels 3/3/3/3.', flush=True)
            shown = 0
            while True:
                last = stream(['valid','actual_mode','sample_age_us',*levels])
                if last[0] and last[1] == 12 and last[2] < 1000000 and last[3:] == [3,3,3,3]: break
                if time.monotonic() >= shown:
                    left = max(0, deadline-time.monotonic())
                    print('levels sys/gyro/accel/mag: {}/{}/{}/{} ({:.0f} s left)'.format(*last[3:], left), flush=True)
                    shown = time.monotonic()+5
                if time.monotonic() >= deadline:
                    raise AssertionError(('real sensor not fully calibrated; rerun with --calibration-wait SECONDS',last))
                time.sleep(.25)
        # CONFIG prevents the sensor's automatic calibration routine from
        # replacing offsets/radii during exact hardware readback checks.
        action('set_mode', 0)
        # Never overwrite the physical sensor with synthetic calibration.
        initial = action('calibration_read')['blob_hex']
        assert len(bytes.fromhex(initial)) == 42
        if modules == 'mock':
            payload = struct.pack('<BB20h', 1, 1, *range(1,21)).hex()
        else:
            payload = initial
        action('calibration_write', '--blob', payload)
        wait_dirty(True)
        cal_refs = [ref(n) for n in items if n.startswith('calibration.')]
        before = cli('get', *cal_refs)['typed_values']
        action('calibration_write', '--blob', '00', code=5)
        assert cli('get', *cal_refs)['typed_values'] == before
        action('calibration_apply')
        applied = action('calibration_read')['blob_hex']
        assert applied == payload, (applied, payload)
        # Restore at boot into CONFIG too, so readback proves application
        # before the fusion algorithm is permitted to recalibrate.
        cli('set', f'{ref("startup_mode")}=0')
        cli.reboot()
        wait_dirty(False)
        assert cli('get', *cal_refs)['typed_values'] == before
        assert action('calibration_read')['blob_hex'] == payload
        # CONFIG exposes invalid data; a paused mode change restores streaming.
        action('set_mode', 0)
        assert stream(['valid','actual_mode'])[0:2] == [False, 0]
        action('set_mode', 12)
        assert stream(['valid','actual_mode'])[0:2] == [True, 12]
        print('PASS IMU signals, paused calibration, rejection, SAVE/reboot restore and modes')
        if stack_wait: time.sleep(stack_wait)
    finally:
        if instance is not None:
            cli('disarm'); cli('sub-clear', 0); cli('cmd-clear', 0)
            cli('remove', type_, instance)
            cli('set', *(f'{r}={v}' for r,v in zip(refs, settings)))
            cli.reboot()
            assert cli('instances') == original
            assert cli('get', *refs)['typed_values'] == settings
            print('PASS IMU smoke restored original layout and settings')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--modules', choices=['mock','real'], default='mock')
    parser.add_argument('--missing-sensor', action='store_true')
    parser.add_argument('--sda', type=int, default=8)
    parser.add_argument('--scl', type=int, default=9)
    parser.add_argument('--address', type=lambda s:int(s,0), default=0x28)
    parser.add_argument('--stack-wait', type=float, default=0, help='hold the test layout for console stack observation')
    parser.add_argument('--calibration-wait', type=float, default=0, help='seconds allowed for physical calibration to reach 3/3/3/3 (real sensor only)')
    args = parser.parse_args()
    try:
        if any(not math.isfinite(v) or not 0 <= v <= 3600 for v in (args.stack_wait,args.calibration_wait)):
            parser.error('waits must be finite and between 0 and 3600 seconds')
        run(args.cli,args.endpoint,args.modules,args.missing_sensor,args.sda,args.scl,args.address,args.stack_wait,args.calibration_wait)
    except (AssertionError,RuntimeError) as exc:
        sys.exit(f'imu_smoke.py: {exc}')
