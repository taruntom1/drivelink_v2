#!/usr/bin/env python3
"""Build and flash bench or normal firmware on the bare board (never validation).

Run from any directory. Full command output goes to build/bench_fw/<mode>.log.
After bench checks, always run `python tools/bench_fw.py normal` from the repo
root to restore normal firmware. Both commands default to protocol UART COM3.
"""
import argparse
from pathlib import Path
import re
import sys

import validate


FIRMWARE = validate.ROOT / 'firmware'
OUT = validate.ROOT / 'build' / 'bench_fw'
BENCH = ['-B', 'build_bench', '-D', 'SDKCONFIG=build_bench/sdkconfig',
         '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench']
NORMAL = ['-B', 'build', '-D', 'SDKCONFIG=sdkconfig',
          '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults']
XIP = ['-B', 'build_xip', '-D', 'SDKCONFIG=build_xip/sdkconfig',
       '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xip']


def check_config(mode):
    folder = {'bench': 'build_bench', 'normal': 'build', 'xip': 'build_xip'}[mode]
    header = FIRMWARE / folder / 'config' / 'sdkconfig.h'
    enabled = bool(re.search(r'^\s*#\s*define\s+CONFIG_CORE_BENCH_CRASH\s+1\b',
                             header.read_text(encoding='utf-8'), re.MULTILINE))
    if enabled != (mode == 'bench'):
        raise validate.Failed(f'{header}: CONFIG_CORE_BENCH_CRASH must be '
                              + ('enabled for bench' if mode == 'bench' else 'off for normal'))
    content = header.read_text(encoding='utf-8')
    for option in ('SPIRAM_XIP_FROM_PSRAM', 'SPIRAM_FETCH_INSTRUCTIONS', 'SPIRAM_RODATA'):
        present = bool(re.search(r'^\s*#\s*define\s+CONFIG_' + option + r'\s+1\b', content, re.MULTILINE))
        if present != (mode == 'xip'):
            raise validate.Failed(f'{header}: CONFIG_{option} must be ' + ('on for xip' if mode == 'xip' else 'off'))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    for mode, help_text in [('bench', 'build isolated bench firmware and flash'),
                            ('normal', 'reconfigure, build, verify bench/XiP are off, and flash'),
                            ('xip', 'build isolated PSRAM XiP evaluation firmware and flash; restore normal afterward')]:
        command = sub.add_parser(mode, help=help_text)
        command.add_argument('--port', default='COM3', help='flash port (default: COM3)')
        command.add_argument('--dry-run', action='store_true',
                             help='show commands and config guard without exporting IDF or flashing')
    args = parser.parse_args(argv)
    options = {'bench': BENCH, 'normal': NORMAL, 'xip': XIP}[args.mode]
    actions = ['build'] if args.mode == 'bench' else ['reconfigure', 'build']
    build = [*options, *actions]
    flash = [*options, '-p', args.port, 'flash']
    if args.dry_run:
        print(f'DRY RUN in {FIRMWARE}')
        print('idf.py ' + ' '.join(f'"{a}"' if ';' in a else a for a in build))
        print(f'Check generated sdkconfig.h: bench {args.mode == "bench"}, XiP {args.mode == "xip"}')
        print('idf.py ' + ' '.join(f'"{a}"' if ';' in a else a for a in flash))
        return 0

    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f'{args.mode}.log'
    with path.open('w', encoding='utf-8') as log:
        try:
            env = validate.idf_env()
            idf = str(Path(env['IDF_PATH']) / 'tools' / 'idf.py')
            for title, command in [('build', build), ('flash', flash)]:
                if title == 'flash':
                    check_config(args.mode)  # fail closed, before opening the port
                    log.write(f'CONFIG_CORE_BENCH_CRASH verified for {args.mode}\n')
                step = validate.Step(title, ['python', idf, *command], cwd=FIRMWARE,
                                     env=env, warning_free=(title == 'build'))
                validate.run_step('bench_fw', step, log, verbose=False)
        except (validate.Unavailable, validate.Failed, OSError) as error:
            log.write(f'FAIL: {error}\n')
            print(f'FAIL {args.mode}: {error}\nLog: {path}')
            return 1
    print(f'PASS {args.mode}: built, config checked, flashed {args.port}\nLog: {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
