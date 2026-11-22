#!/bin/sh
# The ROS build and tests of tools/validate.py `ros` and `ros-jazzy` (and CI's
# ros job): run on Linux with ROS 2 installed (tools/setup_linux.sh ros), from
# the repository root.
#
#   sh tools/ros_check.sh superbuild BUILD_DIR   drivelink-agent, -vdev and -cli (linux-no-gui)
#   sh tools/ros_check.sh build BUILD_DIR        colcon build: protocol, libdrivelink, host/ros
#   sh tools/ros_check.sh test BUILD_DIR         colcon test, then the results
#
# BUILD_DIR holds everything (superbuild/, build/, install/, log/), so the
# checkout is only read. The integration test of drivelink_hardware runs the
# superbuild's programs (DRIVELINK_VDEV, DRIVELINK_AGENT, DRIVELINK_CLI).
set -eu

phase=${1:?usage: ros_check.sh superbuild|build|test BUILD_DIR}
out=${2:?usage: ros_check.sh superbuild|build|test BUILD_DIR}
jobs=${DRIVELINK_JOBS:-$(nproc)}
root=$(pwd)

distro=${ROS_DISTRO:-}
if [ -z "$distro" ]; then
    case "$(. /etc/os-release && echo "${VERSION_CODENAME:-}")" in
        resolute) distro=lyrical ;;
        noble) distro=jazzy ;;
        *) echo "ros_check.sh: no ROS distribution for this Ubuntu release" >&2; exit 2 ;;
    esac
fi
if [ ! -f "/opt/ros/$distro/setup.sh" ]; then
    echo "ros_check.sh: ROS $distro is not installed: sudo sh tools/setup_linux.sh ros" >&2
    exit 2
fi

program() {
    found=$(find "$out/superbuild" -type f -name "$1" -perm -u+x 2>/dev/null | head -n 1)
    if [ -z "$found" ]; then
        echo "ros_check.sh: $1 is not built in $out/superbuild (run the superbuild phase)" >&2
        exit 2
    fi
    echo "$found"
}

case "$phase" in
    superbuild)
        cmake --preset linux-no-gui -S host/superbuild -B "$out/superbuild" -DBUILD_TESTING=OFF
        cmake --build "$out/superbuild" --parallel "$jobs" \
            --target drivelink-agent drivelink-vdev drivelink-cli
        ;;
    build|test)
        # setup.sh reads unset variables.
        set +u
        # shellcheck disable=SC1090
        . "/opt/ros/$distro/setup.sh"
        set -u
        DRIVELINK_VDEV=$(program drivelink-vdev)
        DRIVELINK_AGENT=$(program drivelink-agent)
        DRIVELINK_CLI=$(program drivelink-cli)
        export DRIVELINK_VDEV DRIVELINK_AGENT DRIVELINK_CLI
        export DRIVELINK_HW_INTEGRATION=1
        common="--base-paths $root/protocol $root/host --build-base $out/build --install-base $out/install"
        if [ "$phase" = build ]; then
            # shellcheck disable=SC2086 # $common is a word list
            colcon --log-base "$out/log" build $common --parallel-workers 1 \
                --event-handlers console_direct+ \
                --cmake-args -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
        else
            # The results summary first, then the run's status.
            status=0
            # shellcheck disable=SC2086
            colcon --log-base "$out/log" test $common --event-handlers console_direct+ --return-code-on-test-failure \
                --ctest-args --output-on-failure || status=$?
            colcon --log-base "$out/log" test-result --test-result-base "$out/build" --verbose || status=1
            exit "$status"
        fi
        ;;
    *) echo "usage: ros_check.sh superbuild|build|test BUILD_DIR" >&2; exit 2 ;;
esac
