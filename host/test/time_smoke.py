"""Measure the production clock model on TCP, CH343 or Pi GPIO-UART.

Default gates test estimate stability, not absolute clock accuracy. --jitter-us
100 is the deferred Pi target for the p95 held-out low-RTT midpoint residual.
Use --warmup-only for a short workflow dry run. Raw production timestamps and
model snapshots are retained in --report for independent inspection.
"""
import argparse
import json
from pathlib import Path
import subprocess
from motor_smoke import Device, preflight


def percentile(values, fraction=.95):
    ordered = sorted(values)
    assert ordered, 'no mature measurements'
    return ordered[min(len(ordered)-1, int((len(ordered)-1)*fraction))]


def run(args):
    cli = Device(args.cli, args.endpoint)
    preflight(cli)  # fail before mutation on dirty/configured/armed devices
    assert cli('status')['arm_state']==0, 'requires disarmed device'
    capture=cli('capture-status')
    assert capture['state']=='idle' and not capture['configured'], 'requires unconfigured capture'
    try:
        if args.traffic:
            cli('sub-config', 0, 0, 1, '5:0:256', '5:0:257', '5:0:259')
            cli('sub-enable', 0, 'true')
            cli('capture-config', 0, 1, 1024, 512, 'manual', '5:0:256', '5:0:257', '5:0:259')
            cli('capture-arm')
        result = subprocess.run([args.cli, '--endpoint', args.endpoint, '--json', 'time-sync', str(args.duration)],
                                capture_output=True, text=True, timeout=args.duration+30)
        assert result.returncode == 0, (result.returncode, result.stdout[-2000:], result.stderr[-1000:])
        envelope = json.loads(result.stdout)
        assert envelope['ok'], envelope
        data = envelope['data']
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(data, indent=2)+'\n')
        samples = data['samples']
        assert len(samples) >= 3 and all(s['accepted'] for s in samples), 'invalid/insufficient samples'
        assert len(samples)/(len(samples)+data['timeouts']) >= .95, data['timeouts']
        for s in samples:
            assert s['host_rx_us'] >= s['host_tx_us'] and s['esp_tx_us'] >= s['esp_rx_us']
        residuals, changes = [], []
        for s in samples:
            m = s['model']
            if s['prior_prediction_us'] is None or not m['drift_valid']:
                continue
            # Held-out: prediction was made before inserting this sample.
            # Its processing midpoint maps at prior slope (reported separately).
            if s['rtt_us'] > m['min_rtt_us']*2+100:
                continue
            midpoint = (s['host_tx_us']+s['host_rx_us'])/2
            prior = s['prior_prediction_us'] + s['prior_slope']*(s['esp_tx_us']-s['esp_rx_us'])/2
            current = m['host_reference_us']+m['slope']*(s['esp_rx_us']-m['device_reference_us'])
            residuals.append(abs(midpoint-prior))
            changes.append(abs(current-s['prior_prediction_us']))
        summary = {'samples': len(samples), 'timeouts': data['timeouts'], 'traffic': args.traffic,
                   'rtt_p95_us': percentile([s['rtt_us'] for s in samples]),
                   'model': samples[-1]['model'], 'report': args.report}
        if not args.warmup_only:
            assert len(residuals) >= 8, 'not enough held-out low-RTT samples'
            summary.update(residual_p95_us=percentile(residuals), prediction_change_p95_us=percentile(changes))
            assert samples[-1]['model']['drift_valid'], summary
            assert summary['residual_p95_us'] <= args.jitter_us, summary
            assert summary['prediction_change_p95_us'] <= args.change_us, summary
        if args.traffic:
            cli('capture-trigger'); cli('capture-wait', 15000)
            captured=cli('capture-read')
            rows=[row['typed_values'] for row in captured['rows']]
            summary['device_overruns']=max(row[1] for row in rows)
            summary['longest_tick_us']=max(row[2] for row in rows)
            if args.endpoint.startswith('serial://COM'):
                assert summary['device_overruns']==0, summary
        print('PASS time '+json.dumps(summary))
        return summary
    finally:
        if args.traffic:
            cli('capture-abort');cli('sub-clear',0)
            cli('reboot');cli.connect()  # clears volatile capture; no saved edits


if __name__ == '__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--endpoint', required=True)
    ap.add_argument('--duration', type=int, default=180)
    ap.add_argument('--report', default='build/time-smoke.json')
    ap.add_argument('--traffic', action='store_true')
    ap.add_argument('--warmup-only', action='store_true')
    ap.add_argument('--jitter-us', type=float, default=2000)
    ap.add_argument('--change-us', type=float, default=2000)
    args=ap.parse_args()
    assert 1 <= args.duration <= 3600 and args.jitter_us>0 and args.change_us>0
    run(args)
