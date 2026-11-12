#pragma once

// Host-test stand-in for the generated sdkconfig.h. It defines only the
// symbols that the pure sources under test read, with their Kconfig
// defaults. Keep the values in sync with the components' Kconfig files.

// pid
#define CONFIG_PID_DEFAULT_OUTPUT_MIN -1000
#define CONFIG_PID_DEFAULT_OUTPUT_MAX 1000
#define CONFIG_PID_DEFAULT_DERIVATIVE_CUTOFF_HZ 0
