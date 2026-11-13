#pragma once
// Compatibility for the legacy controller until F5b removes it.
#include "mod_motor/velocity_estimator.hpp"
namespace motor_controller {
using mod_motor::countsToRadians;
using mod_motor::countsPerSecToRadPerSec;
using mod_motor::lowPassAlpha;
using mod_motor::VelocityEstimator;
}
