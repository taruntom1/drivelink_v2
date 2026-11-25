# The image of validate.py's ros-jazzy check: the official ROS image plus the
# `ros` profile of tools/setup_linux.sh (ros2_control, ros2_controllers, colcon).
# Built with tools/ as the context; Docker caches it until setup_linux.sh changes.
ARG DISTRO=jazzy
FROM ros:${DISTRO}
ARG PROFILE=ros
COPY setup_linux.sh /tmp/setup_linux.sh
RUN sh /tmp/setup_linux.sh "$PROFILE" \
    && apt-get update -qq \
    && apt-get install -y --no-install-recommends ccache \
    && rm -rf /var/lib/apt/lists/*
