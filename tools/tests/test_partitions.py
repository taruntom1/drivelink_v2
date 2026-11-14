"""F9 flash layout: retain configuration, two aligned bounded app slots."""
import csv
from pathlib import Path
import unittest


class PartitionTest(unittest.TestCase):
    def test_f9_layout_preserves_nvs_and_fits_flash(self):
        root = Path(__file__).resolve().parents[2]
        rows = [row for row in csv.reader((root / 'firmware/partitions.csv').read_text().splitlines())
                if row and not row[0].startswith('#')]
        parts = {r[0].strip(): (int(r[3], 0), int(r[4], 0)) for r in rows}
        self.assertEqual(parts['nvs'], (0x9000, 0x6000))
        self.assertEqual(parts['otadata'], (0xf000, 0x2000))
        self.assertEqual(parts['phy_init'], (0x11000, 0x1000))
        self.assertEqual(parts['ota_0'], (0x20000, 0x400000))
        self.assertEqual(parts['ota_1'], (0x420000, 0x400000))
        end = 0x9000
        for name, (offset, size) in sorted(parts.items(), key=lambda x: x[1][0]):
            self.assertGreaterEqual(offset, end, name)
            self.assertEqual(offset % (0x10000 if name.startswith('ota_') else 0x1000), 0)
            self.assertEqual(size % 0x1000, 0)
            end = offset + size
        self.assertLessEqual(end, 16 * 1024 * 1024)
