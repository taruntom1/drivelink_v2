# core: the module registry, the loops, the param service, the layout and its
# persistence, and the boot. Pure: every
# source but src/idf/ builds natively against core_os's native backend
# (firmware/test/native). src/idf/core_config_idf.cpp is the only file that
# reads Kconfig.
idf_component_register(
    SRCS "src/capture_service.cpp"
         "src/ota_service.cpp"
         "src/testgen_service.cpp"
         "src/command_service.cpp"
         "src/group_service.cpp"
         "src/source_service.cpp"
         "src/config_blob.cpp"
         "src/core.cpp"
         "src/safety.cpp"
         "src/crc32.cpp"
         "src/instance.cpp"
         "src/layout_validation.cpp"
         "src/loop.cpp"
         "src/loop_instance.cpp"
         "src/param_service.cpp"
         "src/pending_layout.cpp"
         "src/persistence.cpp"
         "src/registry.cpp"
         "src/value.cpp"
         "src/wiring_order.cpp"
         "src/idf/core_config_idf.cpp"
         "src/idf/core_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core_os protocol
    PRIV_REQUIRES uart_comm esp_app_format diag
)
