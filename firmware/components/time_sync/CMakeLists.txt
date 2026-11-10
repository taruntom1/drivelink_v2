idf_component_register(
    SRCS "src/time_sync.cpp"
         "src/time_sync_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES uart_comm esp_timer
    PRIV_REQUIRES protocol rtos_util diag
)
