"""Checks of Bloom's packaging metadata postprocessing."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_common import Failure
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
