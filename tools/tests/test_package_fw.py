"""Hardware-free checks of the firmware packaging helpers (no IDF, no esptool)."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_fw
from package_common import Failure


def image(version='0.1.0+gabc1234', project='drivelink_v2', idf='v5.5.2', magic=0xABCD5432):
    header = bytes([0xE9]) + bytes(23) + bytes(8)  # image header, first segment header
    desc = struct.pack('<IIII', magic, 0, 0, 0)
    desc += version.encode().ljust(32, b'\0') + project.encode().ljust(32, b'\0')
    desc += b'12:00:00'.ljust(16, b'\0') + b'Nov 19 2026'.ljust(16, b'\0') + idf.encode().ljust(32, b'\0')
    return header + desc + bytes(64)


FLASHER = {'flash_files': {'0x0': 'bootloader/bootloader.bin', '0x20000': 'drivelink_v2.bin',
                           '0x8000': 'partition_table/partition-table.bin', '0xf000': 'ota_data_initial.bin'}}


class AppDescTest(unittest.TestCase):
    def test_fields(self):
        self.assertEqual(package_fw.app_desc(image()), ('0.1.0+gabc1234', 'drivelink_v2', 'v5.5.2'))

    def test_a_full_length_version_has_no_terminator(self):
        self.assertEqual(package_fw.app_desc(image(version='x' * 32))[0], 'x' * 32)

    def test_not_an_image(self):
        for data in (b'', bytes(200), image(magic=0x12345678)):
            with self.assertRaises(Failure):
                package_fw.app_desc(data)


class LayoutTest(unittest.TestCase):
    def test_sorted_by_offset(self):
        self.assertEqual([offset for offset, _ in package_fw.flash_layout(FLASHER)],
                         [0x0, 0x8000, 0xf000, 0x20000])

    def test_unexpected_or_missing_files_fail(self):
        extra = {'flash_files': dict(FLASHER['flash_files'], **{'0x420000': 'other.bin'})}
        missing = {'flash_files': {'0x0': 'bootloader/bootloader.bin'}}
        for args in (extra, missing, {}, {'flash_files': {}}):
            with self.assertRaises(Failure):
                package_fw.flash_layout(args)


class ReadmeTest(unittest.TestCase):
    def test_commands_name_the_package_files(self):
        text = package_fw.readme('0.1.0', 'v5.5.2', 'esp32s3',
                                 {'flash_mode': 'dio', 'flash_size': '16MB', 'flash_freq': '80m'},
                                 package_fw.flash_layout(FLASHER))
        self.assertIn('ESP32-S3 (16MB flash)', text)
        self.assertIn('0x0 drivelink-merged.bin', text)
        self.assertIn('0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin '
                      '0x20000 drivelink-app.bin', text)
        self.assertIn('ota-upload drivelink-app.bin', text)
        self.assertIn('overwrites NVS', text)

    def test_package_name_keeps_the_full_version(self):
        self.assertEqual(package_fw.package_name('0.1.0+gabc1234.dirty'),
                         'drivelink-firmware-0.1.0+gabc1234.dirty')


if __name__ == '__main__':
    unittest.main()
