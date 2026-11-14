#include "mod_motor/modules.hpp"

namespace mod_motor {
const std::array<const core::ModuleType *, 2> kHardwareTypes{&kPwmDirType, &kQuadratureType};
}
