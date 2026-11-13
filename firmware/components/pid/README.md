# PID math

`pid_kernel.hpp` contains the pure, nonlogging derivative and output math and
the explicit-config `Kernel` used by motor modules. It allocates nothing.
`track` presets output-domain integral state; the new kernel uses conditional
integration under saturation and derivative on measurement. Its caller owns
all state and supplies elapsed seconds and a symmetric output limit.

`PidController` remains the legacy wrapper with its existing API, configuration
defaults and clamped-integral behavior. It allocates its implementation at
construction and reports invalid configuration through its existing logging.
The shared derivative/output helpers preserve its computation semantics.
Defaults remain in the component's Kconfig; see the public header for the API.
