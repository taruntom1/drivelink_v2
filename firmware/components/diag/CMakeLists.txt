idf_component_register(
    SRCS "src/diag.cpp"
         "src/diag_protocol.cpp"
         "src/diag_reporter.cpp"
         "src/diag_table.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_common freertos log
    PRIV_REQUIRES protocol uart_comm rtos_util esp_timer esp_system esp_app_format
)
