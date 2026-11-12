# core: the module registry, the loops and the param service. Pure: every
# source but src/idf/ builds natively against core_os's native backend
# (firmware/test/native). src/idf/core_config_idf.cpp is the only file that
# reads Kconfig.
idf_component_register(
    SRCS "src/core.cpp"
         "src/instance.cpp"
         "src/loop.cpp"
         "src/param_service.cpp"
         "src/registry.cpp"
         "src/value.cpp"
         "src/idf/core_config_idf.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core_os protocol
)
