import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import guide


class GuideTest(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.site = self.root / 'site'

    def page(self, slug, body=''):
        guide.write(self.site / slug / 'index.html', '<!doctype html><main>' + body + '</main>')

    def minimal_site(self):
        for page in guide.navigation()[1]:
            self.page(page['slug'])
        self.page('assets/third-party')
        guide.write(self.site / 'pagefind/pagefind-entry.json', '{}')
        guide.write(self.site / '.nojekyll', '')

    def test_headings_and_callout(self):
        rendered = guide.markdown().render('# Device\n\n## Device\n\n> **Safety.** Disconnect motors.\n')
        self.assertIn('id="device"', rendered)
        self.assertIn('id="device-2"', rendered)
        self.assertIn('class="callout safety"', rendered)

    def test_protocol_reference_keeps_tables_and_headings(self):
        reference = guide.protocol_reference()
        modules = guide.markdown().render(reference['module_reference'])
        self.assertIn('<h2 id="actuator-pwm-dir">', modules)
        self.assertIn('<th>Range</th>', modules)
        self.assertIn('| 9 | `Fault` |', reference['status_reference'])
        self.assertIn('| 38 | `FaultLatched` |', reference['detail_reference'])
        diagnostic = guide.markdown().render(reference['diagnostic_reference'])
        self.assertIn('<th>Severity</th>', diagnostic)
        self.assertIn('CrcError', diagnostic)

    def test_unknown_page_and_no_search_fail(self):
        self.minimal_site()
        self.page('unlisted')
        with self.assertRaisesRegex(guide.GuideError, 'navigation'):
            guide.check_site(self.site)
        (self.site / 'pagefind/pagefind-entry.json').unlink()
        with self.assertRaisesRegex(guide.GuideError, 'search index'):
            guide.check_site(self.site)

    def test_checks_links_anchors_alt_and_asset_scope(self):
        self.minimal_site()
        self.page('downloads', '<h1 id="files">Files</h1>')
        self.page('', '<a href="/drivelink_v2/downloads/#files">Valid</a>'
                      '<a href="https://example.com/">External link</a>')
        guide.check_site(self.site)
        self.page('', '<a href="/drivelink_v2/downloads/#typo">Wrong anchor</a>'
                      '<a href="/drivelink_v2/invalid/">Wrong path</a>'
                      '<a href="/downloads/">Outside mount</a>'
                      '<script src="//example.com/code.js"></script>'
                      '<img src="https://example.com/photo.png">')
        with self.assertRaises(guide.GuideError) as problem:
            guide.check_site(self.site)
        for message in ('missing anchor', 'missing target', 'leaves the site mount',
                        'missing alt', 'external or unsupported asset'):
            self.assertIn(message, str(problem.exception))

    def test_firmware_manifest_and_esptool_share_input(self):
        firmware = self.root / 'firmware'
        settings = {'flash_mode': 'dio', 'flash_freq': '80m', 'flash_size': '16MB'}
        package = {'version': '1.0.0', 'chip': 'esp32s3', 'flash_settings': settings,
                   'merged': {'offset': '0x0', 'file': guide.MERGED}}
        guide.write(firmware / 'flash_args.json', json.dumps(package))
        (firmware / guide.MERGED).write_bytes(b'firmware fixture')
        info = guide.firmware_info(firmware, '1.0.0', strict=True)
        flash = guide.flash_files(self.site, firmware, info)
        manifest = json.loads(guide.read(self.site / 'flash/manifest.json'))
        self.assertIn('esp-web-install-button', flash)
        self.assertEqual(manifest['version'], '1.0.0')
        self.assertEqual(manifest['builds'], [
            {'chipFamily': 'ESP32-S3', 'parts': [{'path': guide.MERGED, 'offset': 0}]},
        ])
        self.assertEqual((self.site / 'flash' / guide.MERGED).read_bytes(), b'firmware fixture')
        self.assertIn('--flash_size 16MB', guide.esptool_command(info))
        with self.assertRaisesRegex(guide.GuideError, 'does not match'):
            guide.firmware_info(firmware, '2.0.0', strict=True)
        package['merged']['offset'] = '0x20000'
        guide.write(firmware / 'flash_args.json', json.dumps(package))
        with self.assertRaisesRegex(guide.GuideError, 'offset 0'):
            guide.firmware_info(firmware, '1.0.0', strict=True)

    def test_combining_preserves_every_apt_file(self):
        self.minimal_site()
        apt = self.root / 'apt'
        guide.write(apt / 'dists/noble/InRelease', 'signed index')
        guide.write(apt / 'pool/main/d/drive/drive.deb', 'example package')
        guide.write(apt / guide.KEY, 'key')
        guide.write(apt / 'index.html', 'old landing page')
        out = self.root / 'combined'
        guide.combine(self.site, apt, out)
        self.assertEqual(guide.apt_snapshot(apt), guide.apt_snapshot(out))
        self.assertEqual(guide.read(out / 'index.html'), guide.read(self.site / 'index.html'))
        guide.write(self.site / guide.KEY, 'collision')
        with self.assertRaisesRegex(guide.GuideError, 'protected apt path'):
            guide.combine(self.site, apt, self.root / 'another')

    def test_release_requires_firmware_and_screenshots(self):
        with self.assertRaisesRegex(guide.GuideError, 'firmware package'):
            guide.firmware_info(None, '1.0.0', strict=True)
        with self.assertRaisesRegex(guide.GuideError, 'guide screenshots'):
            guide.screenshot(self.site, None, 'connect', strict=True)

    def test_asset_links_use_the_published_names(self):
        self.assertEqual(guide.asset_url('drivelink_1.0.0~ubuntu26.04_amd64.deb', '1.0.0'),
                         guide.PROJECT + '/releases/download/v1.0.0/drivelink_1.0.0.ubuntu26.04_amd64.deb')
