"""Checks of the apt repository helpers (no gpg, no apt, no files)."""
from datetime import datetime, timedelta, timezone
import gzip
import locale
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import apt_repo
from apt_repo import Deb, RepoError

FPR = 'ABCD1234ABCD1234ABCD1234ABCD1234ABCD1234'
DESCRIPTION = ('DriveLink command line client\n'
               ' drivelink-cli configures a device.\n'
               ' .\n'
               ' Second paragraph.')


def make_deb(package='drivelink-cli', version='0.1.0~ubuntu26.04', arch='amd64', path=None, extra=()):
    fields = (('Package', package), ('Version', version), ('Architecture', arch), ('Installed-Size', '12'),
              ('Depends', 'libc6'), *extra, ('Description', DESCRIPTION))
    return Deb(Path(path or f'{package}_{version}_{arch}.deb'), fields, 1234, 'ab' * 32)


def tag_release_debs(version='0.1.0'):
    debs = []
    for release in ('24.04', '26.04'):
        for arch in ('amd64', 'arm64'):
            for package in ('drivelink-cli', 'drivelink-agent') + (('drivelink',) if release == '26.04' else ()):
                debs.append(make_deb(package, f'{version}~ubuntu{release}', arch))
    return debs


class CompleteReleaseTest(unittest.TestCase):
    def setUp(self):
        self.debs = tag_release_debs()
        for release, distro in (('24.04', 'jazzy'), ('26.04', 'lyrical')):
            for arch in ('amd64', 'arm64'):
                for suffix in ('', '-protocol', '-hardware', '-bringup'):
                    self.debs.append(make_deb(f'ros-{distro}-drivelink{suffix}', f'0.1.0~ubuntu{release}', arch))

    def test_complete_release_includes_all_26_assets(self):
        self.assertEqual(len(self.debs), 26)
        apt_repo.check_complete(self.debs)

    def test_correct_count_cannot_mask_a_wrong_distro(self):
        wrong = make_deb('ros-jazzy-drivelink-bringup', '0.1.0~ubuntu26.04', 'arm64')
        with self.assertRaisesRegex(RepoError, 'resolute arm64: missing .*lyrical.*unexpected .*jazzy'):
            apt_repo.check_complete([*self.debs[:-1], wrong])


class VersionTest(unittest.TestCase):
    def test_release_of(self):
        self.assertEqual(apt_repo.release_of('0.1.0~ubuntu26.04'), '26.04')
        self.assertEqual(apt_repo.release_of('0.1.0+g58e1ce8.dirty~ubuntu24.04'), '24.04')

    def test_release_of_needs_the_suffix(self):
        for bad in ('0.1.0', '0.1.0~ubuntu', '0.1.0~debian12', '0.1.0~ubuntu26.04+x', ''):
            with self.assertRaises(RepoError, msg=bad):
                apt_repo.release_of(bad)

    def test_suite_for(self):
        self.assertEqual(apt_repo.suite_for('0.1.0~ubuntu24.04'), 'noble')
        self.assertEqual(apt_repo.suite_for('0.1.0+g247f643~ubuntu26.04'), 'resolute')

    def test_unknown_release_is_named(self):
        with self.assertRaisesRegex(RepoError, '22.04'):
            apt_repo.suite_for('0.1.0~ubuntu22.04')

    def test_base_version(self):
        self.assertEqual(apt_repo.base_version('0.1.0+gabc~ubuntu26.04'), '0.1.0+gabc')
        self.assertEqual(apt_repo.base_version('0.1.0~ubuntu24.04'), '0.1.0')
        with self.assertRaises(RepoError):
            apt_repo.base_version('0.1.0')


class PoolTest(unittest.TestCase):
    def test_pool_path(self):
        self.assertEqual(apt_repo.pool_path('drivelink-cli', 'drivelink-cli_1_amd64.deb'),
                         'pool/main/d/drivelink-cli/drivelink-cli_1_amd64.deb')
        self.assertEqual(apt_repo.pool_path('drivelink', 'x.deb'), 'pool/main/d/drivelink/x.deb')

    def test_lib_packages_use_four_characters(self):
        self.assertEqual(apt_repo.pool_path('libfoo', 'libfoo_1_amd64.deb'), 'pool/main/libf/libfoo/libfoo_1_amd64.deb')
        self.assertEqual(apt_repo.pool_path('library', 'x.deb'), 'pool/main/libr/library/x.deb')


class ControlTest(unittest.TestCase):
    TEXT = ('Package: drivelink-cli\n'
            'Version: 0.1.0~ubuntu26.04\n'
            'Depends: libc6 (>= 2.38), libstdc++6\n'
            'Description: DriveLink command line client\n'
            ' drivelink-cli configures a device.\n'
            ' .\n'
            '\tTabbed line.\n'
            '\n')

    def test_parse_keeps_order_and_continuations(self):
        fields = apt_repo.parse_control(self.TEXT)
        self.assertEqual([name for name, _ in fields], ['Package', 'Version', 'Depends', 'Description'])
        self.assertEqual(dict(fields)['Depends'], 'libc6 (>= 2.38), libstdc++6')
        self.assertEqual(dict(fields)['Description'],
                         'DriveLink command line client\n drivelink-cli configures a device.\n .\n\tTabbed line.')

    def test_a_line_without_a_colon_is_refused(self):
        with self.assertRaises(RepoError):
            apt_repo.parse_control('Package: x\nthis is not a field\n')

    def test_a_leading_continuation_is_refused(self):
        with self.assertRaises(RepoError):
            apt_repo.parse_control(' continued\nPackage: x\n')


class PackagesTest(unittest.TestCase):
    def test_stanza_order_and_dropped_fields(self):
        deb = make_deb(extra=(('Filename', 'old'), ('Size', '1'), ('MD5sum', 'x'), ('SHA1', 'x'), ('SHA256', 'x'),
                              ('SHA512', 'x'), ('Section', 'electronics')))
        text = apt_repo.packages_stanza(deb, 'pool/main/d/drivelink-cli/f.deb')
        self.assertEqual(text, ('Package: drivelink-cli\n'
                                'Version: 0.1.0~ubuntu26.04\n'
                                'Architecture: amd64\n'
                                'Installed-Size: 12\n'
                                'Depends: libc6\n'
                                'Section: electronics\n'
                                'Description: DriveLink command line client\n'
                                ' drivelink-cli configures a device.\n'
                                ' .\n'
                                ' Second paragraph.\n'
                                'Filename: pool/main/d/drivelink-cli/f.deb\n'
                                'Size: 1234\n'
                                f'SHA256: {"ab" * 32}\n'))

    def test_hash_fields_are_dropped_in_any_case(self):
        deb = make_deb(extra=(('sha256', 'dropme'), ('Md5Sum', 'dropme')))
        text = apt_repo.packages_stanza(deb, 'f')
        self.assertEqual(text.count('SHA256:'), 1)
        self.assertNotIn('dropme', text)

    def test_index_sorts_by_package_then_version_and_separates_with_one_blank_line(self):
        a = make_deb('drivelink-agent')
        b = make_deb('drivelink-cli', '0.1.0~ubuntu24.04')
        c = make_deb('drivelink-cli')
        index = apt_repo.packages_index([(c, 'c'), (a, 'a'), (b, 'b')])
        stanzas = index.split('\n\n')
        self.assertEqual(len(stanzas), 3)
        self.assertEqual([s.split('\n', 1)[0] for s in stanzas], ['Package: drivelink-agent'] * 1 + ['Package: drivelink-cli'] * 2)
        self.assertIn('Filename: b\n', stanzas[1])
        self.assertIn('Filename: c\n', stanzas[2])
        self.assertTrue(index.endswith('\n') and not index.endswith('\n\n'))

    def test_empty_index(self):
        self.assertEqual(apt_repo.packages_index([]), '')


class SelectTest(unittest.TestCase):
    def test_the_ten_files_of_a_tag_release(self):
        chosen = apt_repo.select(tag_release_debs())
        self.assertEqual(sorted(chosen), [('noble', 'amd64'), ('noble', 'arm64'), ('resolute', 'amd64'),
                                          ('resolute', 'arm64')])
        self.assertEqual(sum(len(group) for group in chosen.values()), 10)
        self.assertEqual([d.package for d in chosen[('noble', 'amd64')]], ['drivelink-agent', 'drivelink-cli'])
        self.assertEqual([d.package for d in chosen[('resolute', 'arm64')]],
                         ['drivelink', 'drivelink-agent', 'drivelink-cli'])

    def test_a_local_build(self):
        local = [make_deb(p, '0.1.0+g58e1ce8.dirty~ubuntu26.04') for p in ('drivelink', 'drivelink-agent', 'drivelink-cli')]
        self.assertEqual(list(apt_repo.select(local)), [('resolute', 'amd64')])

    def test_no_files(self):
        with self.assertRaises(RepoError):
            apt_repo.select([])

    def test_unknown_architecture_names_the_file(self):
        with self.assertRaisesRegex(RepoError, r'drivelink-cli_0\.1\.0~ubuntu26\.04_armhf\.deb'):
            apt_repo.select([make_deb(arch='armhf')])

    def test_unknown_release_names_the_file(self):
        with self.assertRaisesRegex(RepoError, r'drivelink-cli_0\.1\.0~ubuntu22\.04_amd64\.deb'):
            apt_repo.select([make_deb(version='0.1.0~ubuntu22.04')])

    def test_missing_release_suffix(self):
        with self.assertRaises(RepoError):
            apt_repo.select([make_deb(version='0.1.0')])

    def test_the_file_name_must_match_the_control(self):
        with self.assertRaisesRegex(RepoError, 'renamed.deb'):
            apt_repo.select([make_deb(path='renamed.deb')])
        with self.assertRaisesRegex(RepoError, 'drivelink-cli_0.1.0~ubuntu26.04_arm64.deb'):
            apt_repo.select([make_deb(path='drivelink-cli_0.1.0~ubuntu26.04_amd64.deb', arch='arm64')])

    def test_a_package_twice_in_one_suite_and_arch(self):
        # Two files, different names, same package/suite/arch: 26.04 and 26.04 with another revision.
        first = make_deb(version='0.1.0~ubuntu26.04')
        second = make_deb(version='0.1.0~ubuntu26.04', path=Path('other') / first.path.name)
        with self.assertRaisesRegex(RepoError, 'twice'):
            apt_repo.select([first, second])

    def test_one_package_in_two_suites_is_fine(self):
        chosen = apt_repo.select([make_deb(version='0.1.0~ubuntu24.04'), make_deb(version='0.1.0~ubuntu26.04')])
        self.assertEqual(len(chosen), 2)

    def test_more_than_one_version_is_refused(self):
        debs = [make_deb('drivelink-cli', '0.1.0~ubuntu26.04'), make_deb('drivelink-agent', '0.2.0~ubuntu26.04')]
        with self.assertRaisesRegex(RepoError, r'0\.1\.0.*0\.2\.0'):
            apt_repo.select(debs)

    def test_a_dirty_build_is_another_version(self):
        debs = [make_deb('drivelink-cli', '0.1.0~ubuntu26.04'), make_deb('drivelink-agent', '0.1.0.dirty~ubuntu26.04')]
        with self.assertRaises(RepoError):
            apt_repo.select(debs)


class DateTest(unittest.TestCase):
    def test_format(self):
        date = datetime(2026, 11, 21, 9, 5, 3, tzinfo=timezone.utc)
        self.assertEqual(apt_repo.format_date(date), 'Sat, 21 Nov 2026 09:05:03 UTC')
        self.assertEqual(apt_repo.format_date(datetime(2026, 1, 1)), 'Thu, 01 Jan 2026 00:00:00 UTC')
        self.assertEqual(apt_repo.format_date(datetime(2026, 12, 31, 23, 59, 59)), 'Thu, 31 Dec 2026 23:59:59 UTC')

    def test_other_zones_are_converted(self):
        date = datetime(2026, 11, 21, 23, 0, 0, tzinfo=timezone(timedelta(hours=-5)))
        self.assertEqual(apt_repo.format_date(date), 'Sun, 22 Nov 2026 04:00:00 UTC')

    def test_every_weekday_and_month(self):
        names = [apt_repo.format_date(datetime(2026, 11, 16 + n)).split(',')[0] for n in range(7)]
        self.assertEqual(names, ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'])
        months = [apt_repo.format_date(datetime(2026, n, 1)).split()[2] for n in range(1, 13)]
        self.assertEqual(months, ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'])

    def test_the_locale_does_not_matter(self):
        saved = locale.setlocale(locale.LC_TIME)
        try:
            for name in ('de_DE.UTF-8', 'German_Germany.1252', 'fr_FR.UTF-8'):
                try:
                    locale.setlocale(locale.LC_TIME, name)
                except locale.Error:
                    continue
                self.assertEqual(apt_repo.format_date(datetime(2026, 3, 4, 5, 6, 7)), 'Wed, 04 Mar 2026 05:06:07 UTC')
        finally:
            locale.setlocale(locale.LC_TIME, saved)

    def test_source_date_epoch(self):
        date = apt_repo.source_date({'SOURCE_DATE_EPOCH': '1700000000'})
        self.assertEqual(date, datetime(2023, 11, 14, 22, 13, 20, tzinfo=timezone.utc))
        self.assertEqual(apt_repo.source_date({'SOURCE_DATE_EPOCH': '0'}), datetime(1970, 1, 1, tzinfo=timezone.utc))

    def test_without_the_variable_it_is_now_to_the_second(self):
        before = datetime.now(timezone.utc).replace(microsecond=0)
        date = apt_repo.source_date({})
        self.assertEqual(date.microsecond, 0)
        self.assertEqual(date.utcoffset(), timedelta(0))
        self.assertLessEqual(before, date)
        self.assertLess(date - before, timedelta(seconds=5))

    def test_a_bad_epoch_is_refused(self):
        with self.assertRaises(RepoError):
            apt_repo.source_date({'SOURCE_DATE_EPOCH': 'yesterday'})


class ReleaseTest(unittest.TestCase):
    def test_exact_text(self):
        files = {'main/binary-arm64/Packages.gz': (23, 'b' * 64), 'main/binary-amd64/Packages': (1234567, 'a' * 64),
                 'main/binary-amd64/Packages.gz': (456, 'c' * 64), 'main/binary-arm64/Packages': (0, 'd' * 64)}
        date = datetime(2026, 11, 21, 9, 5, 3, tzinfo=timezone.utc)
        self.assertEqual(apt_repo.release_file('resolute', files, date), (
            'Origin: DriveLink\n'
            'Label: DriveLink\n'
            'Suite: resolute\n'
            'Codename: resolute\n'
            'Version: 26.04\n'
            'Date: Sat, 21 Nov 2026 09:05:03 UTC\n'
            'Architectures: amd64 arm64\n'
            'Components: main\n'
            'Description: DriveLink packages for Ubuntu 26.04\n'
            'SHA256:\n'
            f' {"a" * 64}          1234567 main/binary-amd64/Packages\n'
            f' {"c" * 64}              456 main/binary-amd64/Packages.gz\n'
            f' {"d" * 64}                0 main/binary-arm64/Packages\n'
            f' {"b" * 64}               23 main/binary-arm64/Packages.gz\n'))

    def test_noble(self):
        text = apt_repo.release_file('noble', {}, datetime(2026, 1, 1, tzinfo=timezone.utc))
        self.assertIn('Codename: noble\nVersion: 24.04\n', text)
        self.assertIn('Description: DriveLink packages for Ubuntu 24.04\n', text)
        self.assertTrue(text.endswith('SHA256:\n'))
        self.assertNotIn('Valid-Until', text)
        self.assertNotIn('MD5Sum', text)


class GzipTest(unittest.TestCase):
    def test_equal_input_gives_equal_bytes(self):
        data = b'Package: x\n' * 100
        self.assertEqual(apt_repo.gzip_bytes(data), apt_repo.gzip_bytes(data))

    def test_round_trip_and_empty(self):
        self.assertEqual(gzip.decompress(apt_repo.gzip_bytes(b'abc\n')), b'abc\n')
        self.assertEqual(gzip.decompress(apt_repo.gzip_bytes(b'')), b'')

    def test_header_has_no_timestamp_or_name(self):
        packed = apt_repo.gzip_bytes(b'abc')
        self.assertEqual(packed[3], 0)          # FLG: no file name
        self.assertEqual(packed[4:8], b'\0\0\0\0')  # MTIME


class FingerprintTest(unittest.TestCase):
    def test_normalize(self):
        self.assertEqual(apt_repo.normalize_fingerprint(FPR.lower()), FPR)
        spaced = ' '.join(FPR[i:i + 4] for i in range(0, 40, 4))
        self.assertEqual(apt_repo.normalize_fingerprint(f'  {spaced}\n'), FPR)

    def test_bad_fingerprints(self):
        for bad in ('', FPR[:-1], FPR + '0', 'G' * 40, 'not a key'):
            with self.assertRaises(RepoError, msg=bad):
                apt_repo.normalize_fingerprint(bad)

    def test_check_accepts_other_spellings(self):
        apt_repo.check_fingerprint(FPR, FPR.lower() + '\n')
        apt_repo.check_fingerprint(FPR.lower(), ' '.join(FPR[i:i + 4] for i in range(0, 40, 4)))

    def test_mismatch_names_both(self):
        other = '1' * 40
        with self.assertRaisesRegex(RepoError, f'signing key {other} is not the committed key {FPR}'):
            apt_repo.check_fingerprint(other, FPR)

    def test_a_bad_expected_value_is_refused(self):
        with self.assertRaises(RepoError):
            apt_repo.check_fingerprint(FPR, 'short')


class IndexTest(unittest.TestCase):
    TEMPLATE = '<a href="@URL@/drivelink-archive-keyring.asc">@FINGERPRINT@</a> @URL@\n'

    def test_substitution_and_grouping(self):
        page = apt_repo.render_index(self.TEMPLATE, 'https://example.test/apt', FPR.lower())
        grouped = ' '.join(FPR[i:i + 4] for i in range(0, 40, 4))
        self.assertEqual(page, f'<a href="https://example.test/apt/drivelink-archive-keyring.asc">{grouped}</a> '
                               'https://example.test/apt\n')
        self.assertNotIn('@', page)

    def test_a_missing_placeholder_is_refused(self):
        for template in ('only @URL@', 'only @FINGERPRINT@', 'neither'):
            with self.assertRaises(RepoError, msg=template):
                apt_repo.render_index(template, 'https://x', FPR)

    def test_the_fingerprint_must_be_valid(self):
        with self.assertRaises(RepoError):
            apt_repo.render_index(self.TEMPLATE, 'https://x', 'abc')


class ConstantsTest(unittest.TestCase):
    def test_suites_and_arches(self):
        self.assertEqual(apt_repo.SUITES, {'24.04': 'noble', '26.04': 'resolute'})
        self.assertEqual(apt_repo.ARCHES, ('amd64', 'arm64'))
        self.assertEqual(apt_repo.COMPONENT, 'main')


if __name__ == '__main__':
    unittest.main()
