#pragma once
#include <array>
#include "core/module_type.hpp"
namespace mod_motor {
/// The pure types: `controller.cascade_pid`. The vdev registers these.
extern const std::array<const core::ModuleType *, 1> kTypes;
/// The hardware types: `actuator.pwm_dir` and `feedback.quadrature`. They
/// need a driver backend (`hardware.hpp`): the device's, or a test fake.
extern const std::array<const core::ModuleType *, 2> kHardwareTypes;
extern const core::ModuleType kPwmDirType;
extern const core::ModuleType kQuadratureType;
}
