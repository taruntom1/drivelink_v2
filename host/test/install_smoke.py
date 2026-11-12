"""Install exports, then build a consumer using only find_package."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='drivelink-install-') as temporary:
        root = Path(temporary)
        prefix = root / 'prefix'
        subprocess.run(['cmake', '--install', str(Path(args.build).resolve()), '--prefix', str(prefix)], check=True, stdout=subprocess.DEVNULL)
        source = root / 'consumer'
        source.mkdir()
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(consumer LANGUAGES CXX)
find_package(drivelink REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE drivelink::drivelink)
''')
        (source / 'main.cpp').write_text('''#include <drivelink/client.hpp>
int main() { return drivelink::hex(drivelink::parseHex("ab")) == "ab" ? 0 : 1; }
''')
        command = ['cmake', '-S', str(source), '-B', str(root / 'build'), '-G', 'Ninja', f'-DCMAKE_PREFIX_PATH={prefix}']
        if os.name == 'nt':
            command.append('-DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe')
        subprocess.run(command, check=True)
        subprocess.run(['cmake', '--build', str(root / 'build')], check=True)
        subprocess.run([str(root / 'build' / ('consumer.exe' if os.name == 'nt' else 'consumer'))], check=True)
        print('PASS installed packages with external find_package consumer')


if __name__ == '__main__':
    main()
