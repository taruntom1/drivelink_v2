#!/usr/bin/env python3
"""Render THIRD_PARTY_NOTICES.txt from notices.json and the license texts.

    python host/gui/notices/gen_notices.py --check   # exit 1 if the file differs
    python host/gui/notices/gen_notices.py --write   # rewrite it

Manifest paths (license_files) are relative to this directory. Each distinct
license file appears once, in the appendix, and components refer to it by its
label (the file name without extension). Output is deterministic, UTF-8, LF.
Standard library only.
"""
import argparse
import json
from pathlib import Path
import sys
import textwrap

HERE = Path(__file__).resolve().parent
WIDTH = 80
REQUIRED = ('name', 'version', 'license', 'copyright', 'url', 'license_files', 'dll_patterns')

HEADER = """\
DriveLink third-party notices
=============================

DriveLink bundles the following third-party software. Each component is
listed with its license, copyright and source. The full license texts follow
in the appendix; every component names the texts that apply to it. A standard
license text with a "<year> <copyright holders>" placeholder applies with the
copyright line of the component that names it.

DriveLink's own code is proprietary (see LICENSE.txt). Third-party components
keep their own licenses."""


class NoticesError(Exception):
    pass


def load_manifest(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise NoticesError(f'cannot read {path}: {error}') from error
    components = data.get('components') if isinstance(data, dict) else None
    if not isinstance(components, list) or not components:
        raise NoticesError(f'{path}: "components" must be a non-empty list')
    for index, item in enumerate(components):
        label = item.get('name', f'#{index}') if isinstance(item, dict) else f'#{index}'
        if not isinstance(item, dict):
            raise NoticesError(f'{path}: component {label} is not an object')
        missing = [key for key in REQUIRED if key not in item]
        if missing:
            raise NoticesError(f'{path}: component {label} lacks ' + ', '.join(missing))
        if not item['license_files']:
            raise NoticesError(f'{path}: component {label} names no license file')
    return components


def read_license(path):
    """License text with LF endings, no trailing blanks, no BOM."""
    try:
        text = Path(path).read_bytes().decode('utf-8-sig')
    except (OSError, ValueError) as error:
        raise NoticesError(f'cannot read license file {path}: {error}') from error
    lines = [line.rstrip() for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
    return '\n'.join(lines).strip('\n')


def wrapped(text, indent='', head=None):
    """Wrap each line of text to WIDTH; head leads the first line."""
    out = []
    for line in str(text).split('\n'):
        lead = head if head is not None and not out else indent
        out += textwrap.wrap(line, WIDTH, initial_indent=lead, subsequent_indent=indent,
                             break_long_words=False, break_on_hyphens=False) or ['']
    return out


def field(label, value, indent='   '):
    return wrapped(value, indent + '  ', f'{indent}{label}: ')


def table(rows):
    widths = [max(len(row[col]) for row in rows) for col in range(3)]
    return [('  '.join(cell.ljust(width) for cell, width in zip(row, widths))).rstrip()
            for row in rows]


def label_of(path):
    return Path(path).stem


def render(manifest, base=HERE):
    components = load_manifest(manifest)
    base = Path(base)
    texts = {}  # resolved path -> (label, text), in first-use order
    labels = {}
    for item in components:
        for rel in item['license_files']:
            path = (base / rel).resolve()
            if path in texts:
                continue
            label = label_of(path)
            if label in labels:
                raise NoticesError(f'license files {labels[label]} and {path} share the label {label}')
            labels[label] = path
            texts[path] = (label, read_license(path))

    out = [HEADER, '', 'Summary', '-------', '']
    out += table([('Component', 'Version', 'License')]
                 + [(c['name'], c['version'], c['license']) for c in components])
    out += ['', '', 'Components', '----------']
    for number, item in enumerate(components, 1):
        out += ['', *wrapped(f'{number}. {item["name"]} {item["version"]}', '   ', '')]
        out += field('License', item['license'])
        out += field('Copyright', item['copyright'])
        out += field('Source', item['url'])
        shown = ', '.join('[' + texts[(base / rel).resolve()][0] + ']' for rel in item['license_files'])
        out += field('License text', f'{shown} in the appendix')
        if item.get('note'):
            out += ['', *wrapped(item['note'], '   ')]
    out += ['', '', 'Appendix: license texts', '-----------------------']
    for label, text in texts.values():
        out += ['', '', f'[{label}]', '=' * (len(label) + 2), '', text]
    return '\n'.join(out) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--check', action='store_true', help='exit 1 when the output file differs')
    mode.add_argument('--write', action='store_true', help='rewrite the output file')
    parser.add_argument('--manifest', type=Path, default=HERE / 'notices.json')
    parser.add_argument('--output', type=Path, default=HERE / 'THIRD_PARTY_NOTICES.txt')
    args = parser.parse_args(argv)
    try:
        expected = render(args.manifest, args.manifest.resolve().parent)
    except NoticesError as error:
        print(f'gen_notices: {error}', file=sys.stderr)
        return 2
    data = expected.encode('utf-8')
    if args.write:
        args.output.write_bytes(data)
        print(f'wrote {args.output} ({len(data)} bytes)')
        return 0
    current = args.output.read_bytes() if args.output.is_file() else None
    # A Windows checkout (core.autocrlf) has CRLF; only the text matters.
    if current is None or current.replace(b'\r\n', b'\n') != data:
        print(f'gen_notices: {args.output} is out of date; run gen_notices.py --write',
              file=sys.stderr)
        return 1
    print(f'{args.output.name} is up to date')
    return 0


if __name__ == '__main__':
    sys.exit(main())
