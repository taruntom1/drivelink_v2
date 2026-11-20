#!/bin/sh
# The one list of Linux packages for DriveLink development and CI
# (Ubuntu 26.04: the WSL distro, the ubuntu:26.04 CI containers; the `agent`
# profile also on Ubuntu 24.04, where CI builds the packages without the GUI).
#
#   sudo sh tools/setup_linux.sh [base|agent|host]   (default: host)
#   sh tools/setup_linux.sh --check [base|agent|host] report missing packages, install nothing
#
#   base  compilers, CMake, Ninja, Python 3 and PyYAML: the generator and
#         firmware native tests (CI job native-tests)
#   agent the same package list as base, named for what it builds: the host
#         superbuild without the GUI, so drivelink-agent and its tests
#         (validate.py agent, CI job agent) and the 24.04 packages (deb)
#   host  base plus Ubuntu's Qt 6 (6.10, the GUI's API floor) for the host
#         superbuild with the GUI and its offscreen Qt Quick tests
#         (validate.py host-linux, CI job host-linux)
#
# Packages already installed are skipped; nothing is removed or upgraded.
# Later steps add their profiles here (ROS in H4a). ci.yml holds no package list.
set -eu

check=0
if [ "${1:-}" = "--check" ]; then
    check=1
    shift
fi
profile=${1:-host}

base="build-essential cmake ninja-build python3 python3-yaml git ca-certificates gnupg"
qt="qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
qt6-svg-dev qt6-qpa-plugins libgl-dev \
qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-templates \
qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtqml-workerscript \
qml6-module-qtquick-dialogs qml6-module-qttest"

case "$profile" in
    base|agent) wanted="$base" ;;
    host) wanted="$base $qt" ;;
    *) echo "usage: $0 [base|agent|host]" >&2; exit 2 ;;
esac

missing=""
for package in $wanted; do
    if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed'; then
        missing="$missing $package"
    fi
done

if [ -z "$missing" ]; then
    echo "setup_linux.sh: every '$profile' package is installed"
    exit 0
fi

if [ "$check" -eq 1 ] || [ "$(id -u)" -ne 0 ]; then
    echo "setup_linux.sh: missing:$missing" >&2
    echo "setup_linux.sh: run as root: sudo sh tools/setup_linux.sh $profile" >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
# shellcheck disable=SC2086 # word splitting of the package list is intended
apt-get install -y --no-install-recommends $missing
echo "setup_linux.sh: installed:$missing"
