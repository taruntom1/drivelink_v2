"""Hardware-free checks of the Windows packaging helpers (no builds, no ISCC)."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_win

MANIFEST = {'components': [
    {'name': 'Qt', 'dll_patterns': ['Qt6*.dll', 'qwindows.dll', 'qtquick2plugin.dll']},
    {'name': 'MinGW', 'dll_patterns': ['libgcc_s_seh-1.dll', 'libstdc++-6.dll']},
    {'name': 'Fonts', 'dll_patterns': []},
]}


class UnmappedTest(unittest.TestCase):
    def test_every_mapped_dll_passes(self):
        files = ['Qt6Core.dll', 'platforms/qwindows.dll', 'qml/QtQuick/qtquick2plugin.dll',
                 'libstdc++-6.dll']
        self.assertEqual(package_win.unmapped(files, MANIFEST), [])

    def test_matching_ignores_case(self):
        self.assertEqual(package_win.unmapped(['QT6CORE.DLL', 'platforms/QWindows.dll'], MANIFEST), [])
        self.assertEqual(package_win.unmapped(['Qt6Core.dll'], {'components': [
            {'dll_patterns': ['QT6*.DLL']}]}), [])

    def test_unknown_dll_is_reported_with_its_path(self):
        files = ['Qt6Core.dll', 'plugins/tls/qopensslbackend.dll', 'libwinpthread-1.dll']
        self.assertEqual(package_win.unmapped(files, MANIFEST),
                         ['libwinpthread-1.dll', 'plugins/tls/qopensslbackend.dll'])

    def test_pattern_applies_to_the_file_name_not_the_directory(self):
        self.assertEqual(package_win.unmapped(['Qt6/other.dll'], MANIFEST), ['Qt6/other.dll'])
        self.assertEqual(package_win.unmapped(['a\\b\\qwindows.dll'], MANIFEST), [])

    def test_only_dlls_count(self):
        files = ['drivelink-gui.exe', 'qtquickcontrols2.conf', 'qml/QtQuick/qmldir', 'Qt6Core.dll']
        self.assertEqual(package_win.unmapped(files, MANIFEST), [])

    def test_accepts_a_bare_component_list(self):
        self.assertEqual(package_win.unmapped(['x.dll'], MANIFEST['components']), ['x.dll'])

    def test_staged_dlls_walks_every_depth(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ['a.dll', 'plugins/x/B.DLL', 'qml/q/r/c.dll', 'qml/q/qmldir', 'app.exe']:
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(b'')
            self.assertEqual(package_win.staged_dlls(root),
                             ['a.dll', 'plugins/x/B.DLL', 'qml/q/r/c.dll'])


class CleanEnvTest(unittest.TestCase):
    BASE = {'PATH': 'C:\\Qt\\Tools\\mingw1310_64\\bin;C:\\msys64\\ucrt64\\bin;C:\\Windows',
            'QT_PLUGIN_PATH': 'x', 'qt_qpa_platform': 'y', 'QML2_IMPORT_PATH': 'z',
            'QML_IMPORT_PATH': 'w', 'SystemRoot': 'D:\\Win', 'TEMP': 'C:\\tmp'}

    def test_path_is_only_the_windows_directories(self):
        env = package_win.clean_env(self.BASE)
        self.assertEqual(env['PATH'], 'D:\\Win\\System32;D:\\Win')

    def test_qt_and_qml_variables_are_dropped_case_insensitively(self):
        env = package_win.clean_env(self.BASE)
        self.assertEqual(sorted(env), ['PATH', 'SystemRoot', 'TEMP'])

    def test_default_system_root_and_input_untouched(self):
        base = {'Path': 'C:\\Qt', 'FOO': '1'}
        env = package_win.clean_env(base)
        self.assertEqual(env, {'FOO': '1', 'PATH': 'C:\\Windows\\System32;C:\\Windows'})
        self.assertEqual(base, {'Path': 'C:\\Qt', 'FOO': '1'})


class FindIsccTest(unittest.TestCase):
    ENV = {'LOCALAPPDATA': 'L', 'ProgramFiles(x86)': 'X', 'ProgramFiles': 'P'}

    def find(self, present, explicit=None, on_path=None):
        return package_win.find_iscc(explicit, which=lambda name: on_path, environ=self.ENV,
                                     is_file=lambda path: str(path) in present)

    def test_explicit_wins_over_everything(self):
        found = self.find({'E/ISCC.exe', 'L/Programs/Inno Setup 6/ISCC.exe'}, 'E/ISCC.exe', 'path/ISCC')
        self.assertEqual(found, 'E/ISCC.exe')

    def test_a_missing_explicit_path_is_not_replaced(self):
        self.assertIsNone(self.find({'L/Programs/Inno Setup 6/ISCC.exe'}, 'nope/ISCC.exe'))

    def test_path_before_the_install_folders(self):
        self.assertEqual(self.find({'L/Programs/Inno Setup 6/ISCC.exe'}, on_path='path/ISCC'),
                         'path/ISCC')

    def test_per_user_before_system_folders(self):
        present = {'L/Programs/Inno Setup 6/ISCC.exe', 'X/Inno Setup 6/ISCC.exe'}
        self.assertEqual(self.find(present), 'L/Programs/Inno Setup 6/ISCC.exe')
        self.assertEqual(self.find({'X/Inno Setup 6/ISCC.exe', 'P/Inno Setup 6/ISCC.exe'}),
                         'X/Inno Setup 6/ISCC.exe')
        self.assertEqual(self.find({'P/Inno Setup 6/ISCC.exe'}), 'P/Inno Setup 6/ISCC.exe')

    def test_none_when_nothing_exists(self):
        self.assertIsNone(self.find(set()))


class OutputNameTest(unittest.TestCase):
    def test_name_keeps_the_full_version(self):
        self.assertEqual(package_win.output_name('0.1.0'), 'DriveLink-0.1.0-setup.exe')
        self.assertEqual(package_win.output_name('0.1.0+g1da9074.dirty'),
                         'DriveLink-0.1.0+g1da9074.dirty-setup.exe')

    def test_name_matches_the_script_pattern(self):
        script = package_win.SCRIPT.read_text(encoding='utf-8')
        self.assertIn('OutputBaseFilename=DriveLink-{#AppVersion}-setup', script)


if __name__ == '__main__':
    unittest.main()
