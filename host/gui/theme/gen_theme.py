#!/usr/bin/env python3
"""Write the DriveLink QML Theme singleton from the exported Figma tokens.

Figma is the source of truth. ``export_tokens.js`` (run read-only through the
Figma MCP) returns the variables, text styles and effect styles as JSON:

    python host/gui/theme/gen_theme.py --import exported.json   # canonical tokens.json
    python host/gui/theme/gen_theme.py                          # tokens.json -> Theme.qml
    python host/gui/theme/gen_theme.py --check                  # fail if either is stale

Theme.qml is committed and never edited by hand. Colour variables of the
"Color" collection have a Dark and a Light value; ``Theme.dark`` selects one.
Primitives stay private (aliases are resolved). Size and Type become numbers
and strings, text styles become ``font`` values plus a line height, and effect
styles become shadow parameters.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOKENS = HERE / 'tokens.json'
OUTPUT = HERE / 'Theme.qml'

COLOR_COLLECTION = 'Color'
COLOR_MODES = ('Dark', 'Light')
PRIMITIVES = 'Primitives'
SCALAR_COLLECTIONS = ('Size', 'Type')

# Figma gives two variables the QML name Theme.borderFocus (the focus colour
# and the focus ring width). Explicit, reviewed renames resolve such clashes;
# any other clash fails the generator.
RENAMES = {('Size', 'border/focus'): 'borderFocusWidth'}

WEIGHTS = {'Regular': 'Font.Normal', 'Medium': 'Font.Medium',
           'Semi Bold': 'Font.DemiBold', 'Bold': 'Font.Bold'}

QML_NAME = re.compile(r'Theme\.([A-Za-z][A-Za-z0-9]*)')


class TokenError(ValueError):
    pass


def camel(parts):
    words = [w for part in parts for w in re.split(r'[^A-Za-z0-9]+', part) if w]
    if not words:
        raise TokenError(f'cannot derive a QML name from {parts!r}')
    return words[0].lower() + ''.join(w[:1].upper() + w[1:] for w in words[1:])


def qml_name(collection, variable):
    """The QML property name: the rename table, the description, or the path."""
    key = (collection, variable['name'])
    if key in RENAMES:
        return RENAMES[key]
    match = QML_NAME.search(variable.get('description') or '')
    if match:
        return match.group(1)
    parts = variable['name'].split('/')
    if collection == COLOR_COLLECTION and parts[0] == 'color':
        parts = parts[1:]
    return camel(parts)


def hex_color(rgba):
    channels = []
    for key in ('r', 'g', 'b', 'a'):
        value = rgba[key]
        if not 0 <= value <= 1:
            raise TokenError(f'colour channel {key}={value} outside 0..1')
        channels.append(round(value * 255))
    r, g, b, a = channels
    return f'#{r:02x}{g:02x}{b:02x}' if a == 255 else f'#{a:02x}{r:02x}{g:02x}{b:02x}'


def collections(tokens):
    found = {c['name']: c for c in tokens['collections']}
    for name in (PRIMITIVES, COLOR_COLLECTION, *SCALAR_COLLECTIONS):
        if name not in found:
            raise TokenError(f'missing collection {name!r}')
    return found


def resolve(value, primitives, seen=()):
    if isinstance(value, dict) and 'alias' in value:
        target = value['alias']
        if target in seen:
            raise TokenError(f'alias cycle through {target!r}')
        if target not in primitives:
            raise TokenError(f'unknown alias {target!r}')
        return resolve(primitives[target], primitives, (*seen, target))
    return value


def properties(tokens):
    """[(section, qml name, qml type, expression)] in a stable order."""
    found = collections(tokens)
    primitives = {}
    for variable in found[PRIMITIVES]['variables']:
        modes = list(variable['values'].values())
        if len(modes) != 1:
            raise TokenError(f'primitive {variable["name"]!r} needs exactly one mode')
        primitives[variable['name']] = modes[0]

    color = found[COLOR_COLLECTION]
    if tuple(color['modes']) != COLOR_MODES:
        raise TokenError(f'{COLOR_COLLECTION} modes must be {COLOR_MODES}, got {color["modes"]}')
    out = []
    for variable in sorted(color['variables'], key=lambda v: v['name']):
        if variable['type'] != 'COLOR':
            raise TokenError(f'{variable["name"]!r} in {COLOR_COLLECTION} is not a colour')
        values = {}
        for mode in COLOR_MODES:
            if mode not in variable['values']:
                raise TokenError(f'{variable["name"]!r} has no {mode} value')
            values[mode] = hex_color(resolve(variable['values'][mode], primitives))
        name = qml_name(COLOR_COLLECTION, variable)
        if values['Dark'] == values['Light']:
            out.append(('Colour', name, 'color', f'"{values["Dark"]}"'))
        else:
            out.append(('Colour', name, 'color', f'dark ? "{values["Dark"]}" : "{values["Light"]}"'))

    for collection in SCALAR_COLLECTIONS:
        for variable in sorted(found[collection]['variables'], key=lambda v: v['name']):
            modes = list(variable['values'].values())
            if len(modes) != 1:
                raise TokenError(f'{variable["name"]!r} needs exactly one mode')
            value = resolve(modes[0], primitives)
            name = qml_name(collection, variable)
            if variable['type'] == 'FLOAT':
                out.append((collection, name, 'real', fmt_number(value)))
            elif variable['type'] == 'STRING':
                out.append((collection, name, 'string', json.dumps(value)))
            else:
                raise TokenError(f'unsupported type {variable["type"]} for {variable["name"]!r}')

    for style in tokens.get('textStyles', []):
        out.extend(text_style(style))
    for style in tokens.get('effectStyles', []):
        out.extend(effect_style(style))

    names = {}
    for section, name, *_ in out:
        if name in names:
            raise TokenError(f'QML name {name!r} used by {names[name]} and {section}; add a rename')
        names[name] = section
    return out


def fmt_number(value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise TokenError(f'expected a number, got {value!r}')
    return str(int(value)) if float(value).is_integer() else repr(round(float(value), 4))


def text_style(style):
    base = camel(style['name'].split('/'))
    suffix = base[:1].upper() + base[1:]
    if style['style'] not in WEIGHTS:
        raise TokenError(f'unknown font style {style["style"]!r} in {style["name"]!r}')
    line = style['lineHeight']
    if line.get('unit') != 'PIXELS':
        raise TokenError(f'{style["name"]!r}: line height must be in pixels')
    spacing = style.get('letterSpacing') or {'unit': 'PIXELS', 'value': 0}
    size = style['size']
    letter = spacing['value'] * size / 100 if spacing['unit'] == 'PERCENT' else spacing['value']
    fields = [f'family: {json.dumps(style["family"])}', f'pixelSize: {fmt_number(size)}',
              f'weight: {WEIGHTS[style["style"]]}']
    if letter:
        fields.append(f'letterSpacing: {fmt_number(round(letter, 2))}')
    if 'tnum' in (style.get('description') or ''):
        fields.append('features: { "tnum": 1 }')
    return [('Text style', f'font{suffix}', 'font', f'Qt.font({{ {", ".join(fields)} }})'),
            ('Text style', f'line{suffix}', 'real', fmt_number(line['value']))]


def effect_style(style):
    shadows = [e for e in style['effects'] if e['type'] == 'DROP_SHADOW']
    if len(shadows) != 1 or len(style['effects']) != 1:
        raise TokenError(f'{style["name"]!r}: expected exactly one drop shadow')
    shadow = shadows[0]
    base = camel(style['name'].split('/'))
    return [('Effect', f'{base}Color', 'color', f'"{hex_color(shadow["color"])}"'),
            ('Effect', f'{base}Blur', 'real', fmt_number(shadow['radius'])),
            ('Effect', f'{base}OffsetY', 'real', fmt_number(shadow['offset']['y'])),
            ('Effect', f'{base}Spread', 'real', fmt_number(shadow.get('spread', 0)))]


def render(tokens):
    lines = [
        '// Generated by host/gui/theme/gen_theme.py from tokens.json. Do not edit.',
        f'// Figma file {tokens.get("file", "?")}; re-export with export_tokens.js.',
        'pragma Singleton',
        'import QtQuick',
        '',
        'QtObject {',
        '    // Selects the Dark or Light value of every colour.',
        '    property bool dark: true',
    ]
    section = None
    series = []
    for current, name, kind, expression in properties(tokens):
        if current != section:
            lines += ['', f'    // {current}']
            section = current
        lines.append(f'    readonly property {kind} {name}: {expression}')
        if re.fullmatch(r'series\d+', name):
            series.append(name)
    series.sort(key=lambda n: int(n[6:]))
    if series:
        lines += ['', '    // Plot series in order (series1 first).',
                  f'    readonly property list<color> seriesColors: [{", ".join(series)}]']
    lines += ['}', '']
    return '\n'.join(lines)


def canonical(tokens):
    """tokens.json text: one variable or style per line, stable order."""
    collections(tokens)
    out = ['{', f' "file": {json.dumps(tokens.get("file"))},', ' "collections": [']
    blocks = []
    for collection in tokens['collections']:
        variables = sorted(collection['variables'], key=lambda v: v['name'])
        rows = ',\n'.join('   ' + json.dumps(v, ensure_ascii=False) for v in variables)
        blocks.append(f'  {{"name": {json.dumps(collection["name"])}, '
                      f'"modes": {json.dumps(collection["modes"])}, "variables": [\n{rows}\n  ]}}')
    out.append(',\n'.join(blocks))
    out.append(' ],')
    for key, last in (('textStyles', False), ('effectStyles', True)):
        rows = ',\n'.join('  ' + json.dumps(s, ensure_ascii=False) for s in tokens.get(key, []))
        out.append(f' "{key}": [\n{rows}\n ]' + ('' if last else ','))
    out.append('}')
    return '\n'.join(out) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--import', dest='source', type=Path,
                        help='exported JSON to normalise into tokens.json')
    parser.add_argument('--check', action='store_true', help='fail if a generated file is stale')
    parser.add_argument('--tokens', type=Path, default=TOKENS)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    args = parser.parse_args(argv)
    try:
        if args.source:
            tokens = json.loads(args.source.read_text(encoding='utf-8'))
            args.tokens.write_text(canonical(tokens), encoding='utf-8', newline='\n')
        tokens = json.loads(args.tokens.read_text(encoding='utf-8'))
        expected = {args.tokens: canonical(tokens), args.output: render(tokens)}
    except (OSError, KeyError, TokenError, json.JSONDecodeError) as error:
        print(f'gen_theme: {error}', file=sys.stderr)
        return 1
    stale = []
    for path, text in expected.items():
        current = path.read_text(encoding='utf-8') if path.exists() else None
        if current != text:
            if args.check:
                stale.append(path)
            else:
                path.write_text(text, encoding='utf-8', newline='\n')
    if stale:
        names = ', '.join(p.name for p in stale)
        print(f'gen_theme: {names} out of date; run python host/gui/theme/gen_theme.py',
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
