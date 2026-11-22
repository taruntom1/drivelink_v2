"""Unit tests for gen_theme.py (run by validate.py gen)."""
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_theme  # noqa: E402


def minimal():
    return {
        'file': 'test',
        'collections': [
            {'name': 'Primitives', 'modes': ['Value'], 'variables': [
                {'name': 'gray/0', 'type': 'COLOR', 'description': '',
                 'values': {'Value': {'r': 1, 'g': 1, 'b': 1, 'a': 1}}},
                {'name': 'gray/950', 'type': 'COLOR', 'description': '',
                 'values': {'Value': {'r': 0.0431, 'g': 0.051, 'b': 0.0627, 'a': 1}}},
                {'name': 'scrim/dark', 'type': 'COLOR', 'description': '',
                 'values': {'Value': {'r': 0, 'g': 0, 'b': 0, 'a': 0.6}}},
            ]},
            {'name': 'Color', 'modes': ['Dark', 'Light'], 'variables': [
                {'name': 'color/bg/app', 'type': 'COLOR', 'description': 'QML: Theme.bgApp',
                 'values': {'Dark': {'alias': 'gray/950'}, 'Light': {'alias': 'gray/0'}}},
                {'name': 'color/text/on-accent', 'type': 'COLOR', 'description': '',
                 'values': {'Dark': {'alias': 'gray/0'}, 'Light': {'alias': 'gray/0'}}},
                {'name': 'color/bg/scrim', 'type': 'COLOR', 'description': 'Theme.bgScrim. Dims.',
                 'values': {'Dark': {'alias': 'scrim/dark'}, 'Light': {'alias': 'scrim/dark'}}},
                {'name': 'color/border/focus', 'type': 'COLOR', 'description': 'QML: Theme.borderFocus',
                 'values': {'Dark': {'alias': 'gray/0'}, 'Light': {'alias': 'gray/950'}}},
            ]},
            {'name': 'Size', 'modes': ['Value'], 'variables': [
                {'name': 'border/focus', 'type': 'FLOAT', 'description': 'QML: Theme.borderFocus',
                 'values': {'Value': 2}},
                {'name': 'space/4', 'type': 'FLOAT', 'description': '', 'values': {'Value': 4}},
            ]},
            {'name': 'Type', 'modes': ['Value'], 'variables': [
                {'name': 'type/family/ui', 'type': 'STRING', 'description': 'QML: Theme.typeFamilyUi',
                 'values': {'Value': 'Inter'}},
            ]},
        ],
        'textStyles': [
            {'name': 'Body/Numeric', 'description': 'Tabular numerals (tnum) on.', 'family': 'Inter',
             'style': 'Regular', 'size': 14, 'lineHeight': {'unit': 'PIXELS', 'value': 20},
             'letterSpacing': {'unit': 'PERCENT', 'value': 0}, 'bound': {}},
            {'name': 'Label/Caps', 'description': '', 'family': 'Inter', 'style': 'Semi Bold',
             'size': 11, 'lineHeight': {'unit': 'PIXELS', 'value': 16},
             'letterSpacing': {'unit': 'PERCENT', 'value': 6}, 'bound': {}},
        ],
        'effectStyles': [
            {'name': 'Elevation/Popup', 'description': '', 'effects': [
                {'type': 'DROP_SHADOW', 'radius': 12, 'spread': 0, 'offset': {'x': 0, 'y': 4},
                 'color': {'r': 0, 'g': 0, 'b': 0, 'a': 0.32}}]},
        ],
    }


class GenThemeTest(unittest.TestCase):
    def render(self, tokens=None):
        return gen_theme.render(tokens or minimal())

    def test_aliases_resolve_per_mode(self):
        text = self.render()
        self.assertIn('readonly property color bgApp: dark ? "#0b0d10" : "#ffffff"', text)

    def test_equal_modes_collapse_and_alpha_is_argb(self):
        text = self.render()
        self.assertIn('readonly property color textOnAccent: "#ffffff"', text)
        self.assertIn('readonly property color bgScrim: "#99000000"', text)

    def test_names_from_description_path_and_renames(self):
        text = self.render()
        self.assertIn('property color textOnAccent', text)  # derived from the path
        self.assertIn('property color bgScrim', text)  # "Theme.bgScrim." in prose
        self.assertIn('property color borderFocus', text)
        self.assertIn('readonly property real borderFocusWidth: 2', text)  # reviewed rename
        self.assertIn('readonly property real space4: 4', text)
        self.assertIn('readonly property string typeFamilyUi: "Inter"', text)

    def test_unreviewed_name_clash_fails(self):
        tokens = minimal()
        tokens['collections'][2]['variables'].append(
            {'name': 'space/other', 'type': 'FLOAT', 'description': 'QML: Theme.space4', 'values': {'Value': 5}})
        with self.assertRaisesRegex(gen_theme.TokenError, 'space4'):
            self.render(tokens)

    def test_text_styles(self):
        text = self.render()
        self.assertIn('fontBodyNumeric: Qt.font({ family: "Inter", pixelSize: 14, weight: Font.Normal, '
                      'features: { "tnum": 1 } })', text)
        self.assertIn('readonly property real lineBodyNumeric: 20', text)
        self.assertIn('weight: Font.DemiBold, letterSpacing: 0.66', text)

    def test_effects(self):
        text = self.render()
        self.assertIn('readonly property color elevationPopupColor: "#52000000"', text)
        self.assertIn('readonly property real elevationPopupBlur: 12', text)
        self.assertIn('readonly property real elevationPopupOffsetY: 4', text)

    def test_missing_mode_or_alias_fails(self):
        tokens = minimal()
        del tokens['collections'][1]['variables'][0]['values']['Light']
        with self.assertRaisesRegex(gen_theme.TokenError, 'no Light value'):
            self.render(tokens)
        tokens = minimal()
        tokens['collections'][1]['variables'][0]['values']['Dark'] = {'alias': 'gray/nope'}
        with self.assertRaisesRegex(gen_theme.TokenError, 'unknown alias'):
            self.render(tokens)

    def test_output_is_deterministic_and_order_independent(self):
        tokens = minimal()
        shuffled = copy.deepcopy(tokens)
        for collection in shuffled['collections']:
            collection['variables'].reverse()
        self.assertEqual(self.render(tokens), self.render(shuffled))
        self.assertEqual(gen_theme.canonical(tokens), gen_theme.canonical(shuffled))

    def test_check_reports_stale_and_import_normalises(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exported = tmp / 'exported.json'
            exported.write_text(json.dumps(minimal()), encoding='utf-8')
            tokens, output = tmp / 'tokens.json', tmp / 'Theme.qml'
            args = ['--tokens', str(tokens), '--output', str(output)]
            self.assertEqual(gen_theme.main(['--import', str(exported), *args]), 0)
            self.assertEqual(gen_theme.main(['--check', *args]), 0)
            output.write_text(output.read_text(encoding='utf-8') + '// edited\n', encoding='utf-8')
            self.assertEqual(gen_theme.main(['--check', *args]), 1)
            self.assertEqual(gen_theme.main(args), 0)
            self.assertEqual(gen_theme.main(['--check', *args]), 0)

    def test_committed_theme_is_current(self):
        self.assertEqual(gen_theme.main(['--check']), 0)


if __name__ == '__main__':
    unittest.main()
