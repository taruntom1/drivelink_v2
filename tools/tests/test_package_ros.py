"""Checks of Bloom's packaging metadata postprocessing and caching helpers."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_common import Failure
import package_ros
from package_ros import pin_dependencies


class PackageRosTest(unittest.TestCase):
    def test_pins_build_and_runtime_dependencies_without_editing_other_names(self):
        control = ('Build-Depends: ros-jazzy-drivelink, cmake\n'
                   'Depends: ros-jazzy-drivelink-hardware,\n ros-jazzy-drivelink\n')
        self.assertEqual(pin_dependencies(control, ['ros-jazzy-drivelink'], '0.1.0+g1234567~ubuntu24.04'),
                         'Build-Depends: ros-jazzy-drivelink (= 0.1.0+g1234567~ubuntu24.04), cmake\n'
                         'Depends: ros-jazzy-drivelink-hardware,\n'
                         ' ros-jazzy-drivelink (= 0.1.0+g1234567~ubuntu24.04)\n')

    def test_mismatched_rosdep_rule_is_a_build_failure(self):
        with self.assertRaisesRegex(Failure, 'ros-jazzy-drivelink'):
            pin_dependencies('Depends: ros-lyrical-drivelink\n', ['ros-jazzy-drivelink'], '0.1.0')

    def test_deps_cache_subdir_rotates_with_dependencies_cmake(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            deps_file = root / 'Dependencies.cmake'
            deps_file.write_text('v1', encoding='utf-8')
            first = package_ros.deps_cache_subdir(root / 'cache', deps_file)
            self.assertFalse(package_ros.dependencies_ready(first))
            (first / 'asio-src/asio/include').mkdir(parents=True)
            (first / 'asio-src/asio/include/asio.hpp').write_text('', encoding='utf-8')
            (first / 'mdns-src').mkdir(parents=True)
            (first / 'mdns-src/mdns.h').write_text('', encoding='utf-8')
            self.assertTrue(package_ros.dependencies_ready(first))
            deps_file.write_text('v2', encoding='utf-8')
            second = package_ros.deps_cache_subdir(root / 'cache', deps_file)
            self.assertNotEqual(first, second)
            self.assertFalse(package_ros.dependencies_ready(second))

    def test_ensure_build_depends_skips_apt_when_already_satisfied(self):
        ok = subprocess.CompletedProcess([], 0, stdout='', stderr='')
        missing = subprocess.CompletedProcess([], 1, stdout='', stderr='missing')
        with mock.patch.object(package_ros, 'run', return_value=ok), \
                mock.patch.object(package_ros, 'call') as call:
            self.assertFalse(package_ros.ensure_build_depends(Path('.'), 'cmake', {}, False))
            call.assert_not_called()
        with mock.patch.object(package_ros, 'run', return_value=missing), \
                mock.patch.object(package_ros, 'call') as call:
            self.assertTrue(package_ros.ensure_build_depends(Path('.'), 'cmake', {}, False))
            self.assertEqual(call.call_count, 2)
            self.assertEqual(call.call_args_list[0].args, ('apt-get', 'update', '-qq'))
            self.assertEqual(call.call_args_list[1].args,
                             ('apt-get', '-y', '--no-install-recommends', 'satisfy', 'cmake'))
