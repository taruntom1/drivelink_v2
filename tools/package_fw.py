#!/usr/bin/env python3
"""Package the built ESP32-S3 firmware (firmware/build) for publishing.

    python tools/package_fw.py [--build DIR] [--out DIR]

Run it in the exported ESP-IDF environment after `idf.py build` (validate.py's
fw-package check does both). It reads flasher_args.json, checks that the app
image's embedded version is this checkout's full version and its project name
is the firmware's, then writes build/firmware/drivelink-firmware-<version>/:

  drivelink-merged.bin    one image for an erased or new board, flashed at 0x0
  bootloader.bin, partition-table.bin, ota_data_initial.bin, drivelink-app.bin
                          the parts, at the offsets in flash_args.json
  drivelink-app.bin       also the OTA image (drivelink-cli ota-upload / the GUI)
  drivelink.elf           the app's symbols, for decoding a crash backtrace
  flash_args.json, README.txt, LICENSE.txt, THIRD_PARTY_NOTICES.txt, SHA256SUMS

Exit 0 ok, 1 failure, 2 missing prerequisite. Standard library and esptool
(part of the ESP-IDF environment).
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import version  # noqa: E402
from package_common import Failure, Unavailable, run_logged, say  # noqa: E402

BUILD = ROOT / 'firmware/build'
PROJECT = 'drivelink_v2'
NOTICES = ROOT / 'firmware/notices/THIRD_PARTY_NOTICES.txt'
# Build-relative file -> name in the package.
PARTS = {'bootloader/bootloader.bin': 'bootloader.bin',
         'partition_table/partition-table.bin': 'partition-table.bin',
         'ota_data_initial.bin': 'ota_data_initial.bin',
         f'{PROJECT}.bin': 'drivelink-app.bin'}
MERGED = 'drivelink-merged.bin'
APP_DESC_MAGIC = 0xABCD5432
IMAGE_MAGIC = 0xE9


# Pure helpers

def app_desc(image):
    """(version, project_name, idf_ver) from an ESP app image's esp_app_desc_t.

    The descriptor follows the 24-byte image header and the first segment's
    8-byte header: magic, secure_version, 2 reserved words, version[32],
    project_name[32], time[16], date[16], idf_ver[32].
    """
    if len(image) < 176 or image[0] != IMAGE_MAGIC:
        raise Failure('not an ESP app image')
    magic, = struct.unpack_from('<I', image, 32)
    if magic != APP_DESC_MAGIC:
        raise Failure(f'no app descriptor (magic {magic:#x})')

    def text(offset, size):
        return image[offset:offset + size].split(b'\0', 1)[0].decode('ascii', 'replace')
    return text(48, 32), text(80, 32), text(144, 32)


def flash_layout(args):
    """[(offset int, build-relative file)] from flasher_args.json, sorted by offset."""
    files = args.get('flash_files')
    if not isinstance(files, dict) or not files:
        raise Failure('flasher_args.json has no flash_files')
    layout = sorted((int(offset, 16), path) for offset, path in files.items())
    unknown = [path for _, path in layout if path not in PARTS]
    if unknown or len(layout) != len(PARTS):
        raise Failure(f'unexpected flash files {sorted(files.values())}; expected {sorted(PARTS)}')
    return layout


def package_name(full):
    return f'drivelink-firmware-{full}'


def readme(full, idf, chip, settings, layout):
    parts = ' '.join(f'{offset:#x} {PARTS[path]}' for offset, path in layout)
    flash = (f'--flash_mode {settings["flash_mode"]} --flash_size {settings["flash_size"]} '
             f'--flash_freq {settings["flash_freq"]}')
    name = {'esp32s3': 'ESP32-S3'}.get(chip, chip.upper())
    return f"""DriveLink firmware {full} for the {name} ({settings['flash_size']} flash)
Built with ESP-IDF {idf}.

New board, or a board to reset: the merged image also overwrites NVS (its
gaps are blank), so the saved layout and parameters are lost:
  esptool.py --chip {chip} -p PORT -b 921600 write_flash {flash} 0x0 {MERGED}

The separate parts keep NVS (the saved layout and parameters):
  esptool.py --chip {chip} -p PORT -b 921600 write_flash {flash} {parts}

Update a running board with OTA instead; it keeps NVS and rolls back on a
failed boot (the board must already run the dual-slot partition table):
  drivelink-cli --endpoint serial://PORT ota-upload drivelink-app.bin
or the GUI's Device page. drivelink.elf decodes a crash backtrace
(xtensa-esp32s3-elf-addr2line -e drivelink.elf ADDRESSES). SHA256SUMS lists
every file's SHA-256.
"""


# Steps

def merge(out, chip, settings, layout, build, log):
    argv = [sys.executable, '-m', 'esptool', '--chip', chip, 'merge_bin', '-o', out / MERGED,
            '--flash_mode', settings['flash_mode'], '--flash_size', settings['flash_size'],
            '--flash_freq', settings['flash_freq']]
    for offset, path in layout:
        argv += [f'{offset:#x}', build / path]
    run_logged(argv, log)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--build', type=Path, default=BUILD)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/firmware')
    args = parser.parse_args(argv)
    build, out_root = args.build.resolve(), args.out.resolve()
    try:
        try:
            import esptool  # noqa: F401
        except ImportError as error:
            raise Unavailable('esptool not importable: run in the exported ESP-IDF environment') from error
        flasher = build / 'flasher_args.json'
        if not flasher.is_file():
            raise Unavailable(f'{flasher} missing: build the firmware first (idf.py build)')
        meta = json.loads(flasher.read_text(encoding='utf-8'))
        layout = flash_layout(meta)
        settings = meta['flash_settings']
        chip = meta['extra_esptool_args']['chip']
        full = version.full()
        image = (build / f'{PROJECT}.bin').read_bytes()
        app_version, project, idf = app_desc(image)
        if project != PROJECT:
            raise Failure(f'the app image is {project!r}, expected {PROJECT!r}')
        if app_version != full:
            raise Failure(f'the app image reports {app_version!r} but this checkout is {full!r}; '
                          'reconfigure and rebuild (idf.py reconfigure build)')
        say(f'firmware {full} ({chip}, ESP-IDF {idf})')
        out = out_root / package_name(full)
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        for path, name in PARTS.items():
            shutil.copyfile(build / path, out / name)
        shutil.copyfile(build / f'{PROJECT}.elf', out / 'drivelink.elf')
        merge(out, chip, settings, layout, build, out_root / 'merge_bin.log')
        (out / 'flash_args.json').write_text(json.dumps({
            'version': full, 'chip': chip, 'idf': idf, 'flash_settings': settings,
            'flash_files': {f'{offset:#x}': PARTS[path] for offset, path in layout},
            'merged': {'offset': '0x0', 'file': MERGED}}, indent=2) + '\n', encoding='utf-8')
        (out / 'README.txt').write_text(readme(full, idf, chip, settings, layout), encoding='utf-8')
        (out / 'LICENSE.txt').write_text((ROOT / 'LICENSE').read_text(encoding='utf-8')
                                         .replace('\r\n', '\n'), encoding='utf-8')
        (out / 'THIRD_PARTY_NOTICES.txt').write_text(NOTICES.read_text(encoding='utf-8')
                                                     .replace('\r\n', '\n'), encoding='utf-8')
        sums = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n'
                       for p in sorted(out.iterdir()) if p.is_file())
        (out / 'SHA256SUMS').write_text(sums, encoding='utf-8')
        merged = (out / MERGED).stat().st_size
        say(f'package: {out} ({len(list(out.iterdir()))} files, merged image {merged / 1048576:.2f} MiB)')
    except Unavailable as error:
        print(f'package_fw: unavailable: {error}', file=sys.stderr)
        return 2
    except (Failure, OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        print(f'package_fw: failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
