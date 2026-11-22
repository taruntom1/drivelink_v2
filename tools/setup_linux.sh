#!/bin/sh
# The one list of Linux packages for DriveLink development and CI
# (Ubuntu 26.04: the WSL distro, the ubuntu:26.04 CI containers; the `agent`
# profile also on Ubuntu 24.04, where CI builds the packages without the GUI).
#
#   sudo sh tools/setup_linux.sh [base|agent|host|ros|ros-deb|guide] (default: host)
#   sh tools/setup_linux.sh --check [base|agent|host|ros|ros-deb|guide] report missing packages
#
#   base  compilers, CMake, Ninja, Python 3 and PyYAML: the generator and
#         firmware native tests (CI job native-tests)
#   agent the same package list as base, named for what it builds: the host
#         superbuild without the GUI, so drivelink-agent and its tests
#         (validate.py agent, CI job agent) and the 24.04 packages (deb)
#   host  base plus Ubuntu's Qt 6 (6.10, the GUI's API floor) for the host
#         superbuild with the GUI and its offscreen Qt Quick tests
#         (validate.py host-linux, CI job host-linux)
#   ros   base plus ROS 2 for drivelink_hardware (validate.py ros and
#         ros-jazzy, CI job ros): Lyrical on 26.04 (resolute), Jazzy on 24.04
#         (noble). Adds the ROS apt repository first when it is missing (its
#         key is checked against the published fingerprint); the ros:lyrical
#         and ros:jazzy images already have it.
#   ros-deb  base plus Bloom and Debian tools, in a disposable ROS build container.
#   guide Python and a virtual environment with the pinned static guide tools.
#
# Packages already installed are skipped; nothing is removed or upgraded.
# ci.yml holds no package list.
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

# The ROS distribution for this Ubuntu release (only these two are supported).
ros_distro() {
    codename=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
    case "$codename" in
        resolute) echo lyrical ;;
        noble) echo jazzy ;;
        *) echo "setup_linux.sh: ROS needs Ubuntu 26.04 (Lyrical) or 24.04 (Jazzy), not '$codename'" >&2; exit 2 ;;
    esac
}

case "$profile" in
    base|agent) wanted="$base" ;;
    host) wanted="$base $qt" ;;
    ros)
        distro=$(ros_distro)
        # ros-base: rclcpp, ament and launch; ros2-control/-controllers: the
        # hardware interface, controller_manager and the broadcasters;
        # ros-dev-tools: colcon and rosdep.
        wanted="$base curl ros-$distro-ros-base ros-$distro-ros2-control ros-$distro-ros2-controllers \
ros-$distro-ament-cmake-gtest ros-$distro-xacro ros-$distro-robot-state-publisher ros-dev-tools"
        ;;
    ros-deb) wanted="$base python3-bloom python3-rosdep debhelper fakeroot" ;;
    guide) wanted="python3 python3-venv ca-certificates" ;;
    *) echo "usage: $0 [base|agent|host|ros|ros-deb|guide]" >&2; exit 2 ;;
esac

missing=""
for package in $wanted; do
    if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed'; then
        missing="$missing $package"
    fi
done

if [ -z "$missing" ]; then
    echo "setup_linux.sh: every '$profile' package is installed"
    if [ "$profile" != guide ] || [ "$check" -eq 1 ]; then
        exit 0
    fi
elif [ "$check" -eq 1 ] || [ "$(id -u)" -ne 0 ]; then
    echo "setup_linux.sh: missing:$missing" >&2
    echo "setup_linux.sh: run as root: sudo sh tools/setup_linux.sh $profile" >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive

# The ROS 2 apt repository (https://docs.ros.org), unless a source already names it.
ros_key=/usr/share/keyrings/ros-archive-keyring.gpg
ros_fingerprint=C1CF6E31E6BADE8868B172B4F42ED6FBAB17C654
if { [ "$profile" = ros ] || [ "$profile" = ros-deb ]; } &&
   ! grep -rqsE '^[^#]*packages\.ros\.org/ros2' /etc/apt/sources.list /etc/apt/sources.list.d/; then
    apt-get update
    apt-get install -y --no-install-recommends curl ca-certificates gnupg
    tmp=$(mktemp)
    curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o "$tmp"
    # Refuse anything but exactly the ROS archive key (one primary key, its fingerprint).
    primaries=$(gpg --show-keys --with-colons "$tmp" | awk -F: '$1 == "pub" { want = 1; next } want && $1 == "fpr" { print $10; want = 0 }')
    if [ "$primaries" != "$ros_fingerprint" ]; then
        rm -f "$tmp"
        echo "setup_linux.sh: the downloaded ROS key is not $ros_fingerprint" >&2
        exit 1
    fi
    gpg --dearmor --yes -o "$ros_key" "$tmp"
    rm -f "$tmp"
    codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
    cat > /etc/apt/sources.list.d/ros2.sources <<EOF
Types: deb
URIs: http://packages.ros.org/ros2/ubuntu
Suites: $codename
Components: main
Signed-By: $ros_key
EOF
    echo "setup_linux.sh: added the ROS 2 apt repository for $codename"
fi

if [ -n "$missing" ]; then
    apt-get update
    # shellcheck disable=SC2086 # word splitting of the package list is intended
    apt-get install -y --no-install-recommends $missing
    echo "setup_linux.sh: installed:$missing"
fi

if [ "$profile" = guide ] && [ "$check" -eq 0 ]; then
    root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
    # Install into the invoking developer's worktree, with their ownership.
    if [ -n "${SUDO_USER:-}" ]; then
        runuser -u "$SUDO_USER" -- python3 -m venv "$root/build/guide-venv"
        runuser -u "$SUDO_USER" -- "$root/build/guide-venv/bin/python" -m pip install -r "$root/guide/requirements.txt"
    else
        python3 -m venv "$root/build/guide-venv"
        "$root/build/guide-venv/bin/python" -m pip install -r "$root/guide/requirements.txt"
    fi
fi
