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
set(CMAKE_DISABLE_FIND_PACKAGE_nlohmann_json TRUE)
find_package(drivelink REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE drivelink::drivelink)
''')
        (source / 'main.cpp').write_text('''#include <drivelink/client.hpp>
#include <expected>
#if __cplusplus <= 202002L
#error drivelink must propagate its C++23 requirement to consumers
#endif
int main() {
    std::expected<int, int> value = 42;
    if (value.value() != 42) { return 4; }
    const auto descriptor=drivelink::ValueType::U8;
    if(drivelink::parseValue(descriptor,"42")!=42) { return 1; }
    try {
        drivelink::Session session("invalid-endpoint");
        drivelink::Client client(session);
        client.info();
        return 2;
    } catch(const drivelink::Error &error) {
        return error.kind==drivelink::ErrorKind::Usage ? 0 : 3;
    }
}
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
