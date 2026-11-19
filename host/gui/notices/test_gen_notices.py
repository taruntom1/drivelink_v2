"""Checks of the notices renderer (temporary manifests; no repository files)."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_notices


def component(name, licenses, **extra):
    item = {'name': name, 'version': '1.0', 'license': 'MIT', 'copyright': f'(c) {name}',
            'url': f'https://example.org/{name}', 'license_files': licenses, 'dll_patterns': []}
    item.update(extra)
    return item


class NoticesTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.base = Path(folder.name)
        (self.base / 'licenses').mkdir()
        (self.base / 'licenses' / 'MIT.txt').write_bytes(b'MIT text\r\nline two  \r\n')
        (self.base / 'licenses' / 'ISC.txt').write_text('ISC text\n', encoding='utf-8')
        self.manifest = self.base / 'notices.json'
        self.output = self.base / 'THIRD_PARTY_NOTICES.txt'

    def write_manifest(self, *components):
        self.manifest.write_text(json.dumps({'components': list(components)}), encoding='utf-8')

    def render(self):
        return gen_notices.render(self.manifest, self.base)

    def run_main(self, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return gen_notices.main([*args, '--manifest', str(self.manifest),
                                     '--output', str(self.output)])

    def test_render_is_deterministic_and_normalised(self):
        self.write_manifest(component('A', ['licenses/MIT.txt']), component('B', ['licenses/ISC.txt']))
        first = self.render()
        self.assertEqual(first, self.render())
        self.assertNotIn('\r', first)
        self.assertTrue(first.endswith('\n') and not first.endswith('\n\n'))
        self.assertIn('MIT text\nline two\n', first)
        self.assertIn('1. A 1.0', first)
        self.assertIn('2. B 1.0', first)

    def test_each_license_appears_once_in_the_appendix(self):
        self.write_manifest(component('A', ['licenses/MIT.txt']),
                            component('B', ['licenses/MIT.txt', 'licenses/ISC.txt']),
                            component('C', ['licenses/./MIT.txt']))
        text = self.render()
        self.assertEqual(text.count('MIT text'), 1)
        self.assertEqual(text.count('ISC text'), 1)
        self.assertEqual(text.count('[MIT]\n'), 1)
        self.assertEqual(text.count('License text: [MIT]'), 3)
        self.assertIn('License text: [MIT], [ISC] in the appendix', text)

    def test_summary_lists_every_component(self):
        self.write_manifest(component('A', ['licenses/MIT.txt']),
                            component('Bee', ['licenses/ISC.txt'], license='ISC', version='2.5'))
        text = self.render()
        self.assertIn('Component  Version  License', text)
        self.assertIn('Bee        2.5      ISC', text)

    def test_note_is_included(self):
        self.write_manifest(component('A', ['licenses/MIT.txt'], note='Replaceable DLLs.'))
        self.assertIn('   Replaceable DLLs.', self.render())

    def test_missing_license_file_is_an_error(self):
        self.write_manifest(component('A', ['licenses/GPL.txt']))
        with self.assertRaises(gen_notices.NoticesError):
            self.render()
        self.assertEqual(self.run_main('--write'), 2)
        self.assertFalse(self.output.exists())

    def test_incomplete_component_is_an_error(self):
        broken = component('A', ['licenses/MIT.txt'])
        del broken['url']
        self.write_manifest(broken)
        with self.assertRaises(gen_notices.NoticesError):
            self.render()

    def test_same_label_for_different_files_is_an_error(self):
        (self.base / 'other').mkdir()
        (self.base / 'other' / 'MIT.txt').write_text('different', encoding='utf-8')
        self.write_manifest(component('A', ['licenses/MIT.txt', 'other/MIT.txt']))
        with self.assertRaises(gen_notices.NoticesError):
            self.render()

    def test_check_detects_drift_and_write_repairs_it(self):
        self.write_manifest(component('A', ['licenses/MIT.txt']))
        self.assertEqual(self.run_main('--check'), 1)  # no file yet
        self.assertEqual(self.run_main('--write'), 0)
        self.assertEqual(self.run_main('--check'), 0)
        self.output.write_bytes(self.output.read_bytes() + b'edit\n')
        self.assertEqual(self.run_main('--check'), 1)
        self.assertEqual(self.run_main('--write'), 0)
        self.assertEqual(self.run_main('--check'), 0)
        (self.base / 'licenses' / 'MIT.txt').write_text('MIT text changed\n', encoding='utf-8')
        self.assertEqual(self.run_main('--check'), 1)

    def test_output_uses_lf_endings(self):
        self.write_manifest(component('A', ['licenses/MIT.txt']))
        self.run_main('--write')
        self.assertNotIn(b'\r', self.output.read_bytes())


class CommittedNoticesTest(unittest.TestCase):
    def test_committed_manifest_files_exist_and_output_is_current(self):
        self.assertEqual(gen_notices.render(gen_notices.HERE / 'notices.json'),
                         (gen_notices.HERE / 'THIRD_PARTY_NOTICES.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
