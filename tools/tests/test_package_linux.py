"""Hardware-free checks of the .deb packaging helpers (no builds, no dpkg)."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_linux
from package_common import Failure


class VersionTest(unittest.TestCase):
    def test_full_versions_are_debian_versions(self):
        for full in ('0.1.0', '0.1.0+g1da9074', '0.1.0+g1da9074.dirty'):
            self.assertEqual(package_linux.debian_version(full), full)

    def test_bad_versions_are_refused(self):
        for bad in ('', 'v0.1.0', '0.1.0 beta', '0.1.0_1'):
            with self.assertRaises(Failure, msg=bad):
                package_linux.debian_version(bad)

    def test_file_name(self):
        self.assertEqual(package_linux.deb_name('0.1.0+gabc1234', 'amd64'),
                         'drivelink_0.1.0+gabc1234_amd64.deb')


class DependsTest(unittest.TestCase):
    OUTPUT = ('dpkg-shlibdeps: warning: something\n'
              'shlibs:Depends=libc6 (>= 2.38), libqt6core6t64 (>= 6.10.1), libstdc++6 (>= 14)\n')

    def test_parse(self):
        self.assertEqual(package_linux.parse_shlibdeps(self.OUTPUT),
                         ['libc6 (>= 2.38)', 'libqt6core6t64 (>= 6.10.1)', 'libstdc++6 (>= 14)'])

    def test_no_depends_line_fails(self):
        with self.assertRaises(Failure):
            package_linux.parse_shlibdeps('dpkg-shlibdeps: error: nothing\n')

    def test_merge_keeps_order_and_skips_named_packages(self):
        shlibs = ['libc6 (>= 2.38)', 'qml6-module-qtquick (>= 6.10)']
        self.assertEqual(package_linux.merge_depends(shlibs, ['qml6-module-qtquick', 'qml6-module-qtcore']),
                         ['libc6 (>= 2.38)', 'qml6-module-qtquick (>= 6.10)', 'qml6-module-qtcore'])

    def test_every_qml_import_has_a_package(self):
        # A module the GUI imports must be in Depends, or a clean install fails to load it.
        modules = {'QtQuick': 'qml6-module-qtquick', 'QtQuick.Controls': 'qml6-module-qtquick-controls',
                   'QtQuick.Templates': 'qml6-module-qtquick-templates',
                   'QtQuick.Layouts': 'qml6-module-qtquick-layouts', 'QtQuick.Window': 'qml6-module-qtquick-window',
                   'QtQuick.Dialogs': 'qml6-module-qtquick-dialogs', 'QtCore': 'qml6-module-qtcore',
                   'QtQml.Models': 'qml6-module-qtqml-models', 'QtQml': 'qml6-module-qtqml'}
        root = package_linux.ROOT / 'host/gui'
        imports = set()
        for qml in root.rglob('*.qml'):
            if 'tests' in qml.parts:
                continue
            for line in qml.read_text(encoding='utf-8').splitlines():
                if line.startswith('import Qt') and not line.startswith('import QtTest'):
                    name = line.split()[1]
                    imports.add('QtQuick.Controls' if name.startswith('QtQuick.Controls') else name)
        unknown = sorted(name for name in imports if name not in modules)
        self.assertEqual(unknown, [], 'add the package to QML_PACKAGES and this table')
        for name in imports:
            self.assertIn(modules[name], package_linux.QML_PACKAGES, name)


class ControlTest(unittest.TestCase):
    def test_fields(self):
        text = package_linux.control('0.1.0+gabc1234', 'amd64', ['libc6 (>= 2.38)', 'qml6-module-qtquick'], 1234)
        fields = dict(line.split(': ', 1) for line in text.splitlines() if not line.startswith(' '))
        self.assertEqual(fields['Package'], 'drivelink')
        self.assertEqual(fields['Version'], '0.1.0+gabc1234')
        self.assertEqual(fields['Architecture'], 'amd64')
        self.assertEqual(fields['Installed-Size'], '1234')
        self.assertEqual(fields['Depends'], 'libc6 (>= 2.38), qml6-module-qtquick')
        self.assertTrue(text.endswith('\n'))
        # The synopsis is one line under 80 characters; the long description is indented.
        self.assertLess(len(fields['Description']), 80)
        self.assertTrue(all(line.startswith(' ') for line in text.split('Description: ', 1)[1].splitlines()[1:]))


class BundledTest(unittest.TestCase):
    def test_shared_libraries_at_any_depth(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            for name in ('usr/bin/drivelink-gui', 'usr/lib/libQt6Core.so.6', 'usr/lib/x/libfoo.so',
                         'usr/share/doc/drivelink/copyright', 'usr/lib/notalib.so.txt'):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(b'')
            self.assertEqual(package_linux.shared_libraries(root),
                             ['usr/lib/libQt6Core.so.6', 'usr/lib/x/libfoo.so'])

    def test_lf(self):
        self.assertEqual(package_linux.lf('a\r\nb\n'), 'a\nb\n')


if __name__ == '__main__':
    unittest.main()
