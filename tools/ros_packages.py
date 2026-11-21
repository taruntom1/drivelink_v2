"""DriveLink's ROS Debian package names, targets, and internal dependencies."""

ROS_TARGETS = {'24.04': 'jazzy', '26.04': 'lyrical'}

# Build order matters. Each build sees the previous packages installed via dpkg.
ROS_PACKAGES = {
    'drivelink_protocol': ('protocol', ()),
    'drivelink': ('host/libdrivelink', ('drivelink_protocol',)),
    'drivelink_hardware': ('host/ros/drivelink_hardware', ('drivelink',)),
    'drivelink_bringup': ('host/ros/drivelink_bringup', ('drivelink', 'drivelink_hardware')),
}


def debian_name(distro, package):
    return f'ros-{distro}-{package.replace("_", "-")}'
