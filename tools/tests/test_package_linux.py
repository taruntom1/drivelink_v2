"""Hardware-free checks of the .deb packaging helpers (no builds, no dpkg)."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_linux
from package_common import Failure

CLI, AGENT, GUI = (package_linux.BY_NAME[n] for n in ('drivelink-cli', 'drivelink-agent', 'drivelink'))


def fields_of(text):
    return dict(line.split(': ', 1) for line in text.splitlines() if not line.startswith(' '))


class VersionTest(unittest.TestCase):
    def test_full_versions_are_debian_versions(self):
        for full in ('0.1.0', '0.1.0+g1da9074', '0.1.0+g1da9074.dirty'):
            self.assertEqual(package_linux.debian_version(full), full)

    def test_bad_versions_are_refused(self):
        for bad in ('', 'v0.1.0', '0.1.0 beta', '0.1.0_1'):
            with self.assertRaises(Failure, msg=bad):
                package_linux.debian_version(bad)

    def test_release_suffix(self):
        self.assertEqual(package_linux.package_version('0.1.0+gabc1234', 'ubuntu', '24.04'),
                         '0.1.0+gabc1234~ubuntu24.04')
        self.assertEqual(package_linux.package_version('0.1.0', 'ubuntu', '26.04'), '0.1.0~ubuntu26.04')

    def test_file_name(self):
        self.assertEqual(package_linux.deb_name('drivelink-cli', '0.1.0~ubuntu26.04', 'arm64'),
                         'drivelink-cli_0.1.0~ubuntu26.04_arm64.deb')

    def test_os_release(self):
        text = 'PRETTY_NAME="Ubuntu 26.04 LTS"\nNAME="Ubuntu"\nVERSION_ID="26.04"\nID=ubuntu\nID_LIKE=debian\n'
        self.assertEqual(package_linux.parse_os_release(text), ('ubuntu', '26.04'))
        for bad in ('ID=ubuntu\n', 'VERSION_ID="26.04"\n', 'ID=ubuntu\nVERSION_ID=rolling\n'):
            with self.assertRaises(Failure, msg=bad):
                package_linux.parse_os_release(bad)


class ReleaseTest(unittest.TestCase):
    def test_gui_only_where_qt_meets_the_floor(self):
        self.assertFalse(package_linux.gui_supported('ubuntu', '24.04'))
        self.assertFalse(package_linux.gui_supported('ubuntu', '25.10'))
        self.assertTrue(package_linux.gui_supported('ubuntu', '26.04'))
        self.assertTrue(package_linux.gui_supported('ubuntu', '28.04'))
        self.assertFalse(package_linux.gui_supported('debian', '26.04'))

    def test_packages_per_release(self):
        names = lambda release: [p.name for p in package_linux.packages_for('ubuntu', release)]
        self.assertEqual(names('24.04'), ['drivelink-cli', 'drivelink-agent'])
        self.assertEqual(names('26.04'), ['drivelink-cli', 'drivelink-agent', 'drivelink'])


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

    def test_the_gui_needs_the_cli_at_its_own_version(self):
        depends = package_linux.all_depends(GUI, ['libc6 (>= 2.38)'], '0.1.0~ubuntu26.04')
        self.assertEqual(depends[:2], ['libc6 (>= 2.38)', 'drivelink-cli (= 0.1.0~ubuntu26.04)'])
        self.assertTrue(set(package_linux.QML_PACKAGES) <= set(depends))

    def test_the_agent_and_cli_depends(self):
        self.assertEqual(package_linux.all_depends(AGENT, ['libc6'], 'v'), ['libc6', 'passwd'])
        self.assertEqual(package_linux.all_depends(CLI, ['libc6'], 'v'), ['libc6'])

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
        text = package_linux.control(GUI, '0.1.0+gabc1234~ubuntu26.04', 'amd64',
                                     ['libc6 (>= 2.38)', 'qml6-module-qtquick'], 1234)
        fields = fields_of(text)
        self.assertEqual(fields['Package'], 'drivelink')
        self.assertEqual(fields['Version'], '0.1.0+gabc1234~ubuntu26.04')
        self.assertEqual(fields['Architecture'], 'amd64')
        self.assertEqual(fields['Installed-Size'], '1234')
        self.assertEqual(fields['Depends'], 'libc6 (>= 2.38), qml6-module-qtquick')
        self.assertEqual(fields['Recommends'], 'qt6-wayland')
        self.assertNotIn('Replaces', fields)
        self.assertTrue(text.endswith('\n'))

    def test_every_description_is_well_formed(self):
        # The synopsis is one line under 80 characters; the long description is indented.
        for package in package_linux.PACKAGES:
            text = package_linux.control(package, '1', 'amd64', ['libc6'], 1)
            synopsis, *rest = text.split('Description: ', 1)[1].splitlines()
            self.assertLess(len(synopsis), 80, package.name)
            self.assertTrue(rest and all(line.startswith(' ') and len(line) <= 80 for line in rest), package.name)

    def test_split_fields(self):
        cli = fields_of(package_linux.control(CLI, '1', 'amd64', ['libc6'], 1))
        self.assertEqual(cli['Replaces'], 'drivelink')  # takes over H2g's /usr/bin/drivelink-cli
        self.assertNotIn('Recommends', cli)
        agent = fields_of(package_linux.control(AGENT, '1', 'amd64', ['libc6'], 1))
        self.assertEqual(agent['Recommends'], 'drivelink-cli')
        self.assertNotIn('Replaces', agent)

    def test_no_two_packages_ship_one_file(self):
        owned = {}
        for package in package_linux.PACKAGES:
            paths = [f'usr/bin/{p}' for p in package.programs] + list(package.files) + [
                f'usr/share/{package.name}/LICENSE.txt', f'usr/share/doc/{package.name}/copyright']
            for path in paths:
                self.assertNotIn(path, owned, f'{package.name} and {owned.get(path)}')
                owned[path] = package.name


class AgentPackageTest(unittest.TestCase):
    def test_conffile_is_shipped_and_lists_no_devices(self):
        import json
        self.assertEqual(AGENT.conffiles, ['/etc/drivelink/agent.json'])
        source, mode = AGENT.files['etc/drivelink/agent.json']
        config = json.loads(source.read_text(encoding='utf-8'))
        self.assertEqual(config, {'bind': '127.0.0.1', 'port': 47620, 'token_file': 'token', 'devices': []})

    def test_scripts_take_the_version(self):
        text = package_linux.maintainer_script("V='@VERSION@'\r\n", '0.1.0+gabc~ubuntu26.04')
        self.assertEqual(text, "V='0.1.0+gabc~ubuntu26.04'\n")
        with self.assertRaises(Failure):
            package_linux.maintainer_script("V='@VERSION@'", "1'; rm -rf /")

    def test_upgrade_never_stops_or_restarts_the_agent(self):
        # The safety rule (user decision 2026-11-20): only remove stops it.
        deploy = package_linux.DEPLOY
        prerm = (deploy / 'prerm').read_text(encoding='utf-8')
        upgrade = re.search(r'\n(upgrade\|failed-upgrade)\)\n(.*?);;', prerm, re.S)
        self.assertIsNotNone(upgrade)
        self.assertNotIn('systemctl', upgrade.group(2))
        for script in ('postinst', 'prerm', 'postrm'):
            text = (deploy / script).read_text(encoding='utf-8')
            # Commands only: the notices tell the user to restart.
            commands = '\n'.join(line for line in text.splitlines()
                                 if not line.strip().startswith(('#', 'echo')))
            self.assertNotRegex(commands, r'systemctl\s+(restart|try-restart|reload-or-restart|condrestart)', script)
            self.assertTrue(text.startswith('#!/bin/sh\n'), script)
            self.assertIn('\nset -e\n', text, script)
        postinst = (deploy / 'postinst').read_text(encoding='utf-8')
        self.assertNotIn('systemctl stop', postinst)
        self.assertEqual(postinst.count('systemctl start'), 1)

    def test_unit_runs_the_installed_config_as_drivelink(self):
        unit = (package_linux.DEPLOY / 'drivelink-agent.service').read_text(encoding='utf-8')
        for line in ('User=drivelink', 'SupplementaryGroups=dialout', 'KillSignal=SIGTERM', 'Restart=on-failure',
                     'ExecStart=/usr/bin/drivelink-agent --config /etc/drivelink/agent.json',
                     'RestartPreventExitStatus=2', 'WantedBy=multi-user.target'):
            self.assertIn(f'\n{line}\n', unit)


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
