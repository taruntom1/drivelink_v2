idf_component_register(
    SRCS "time_sync.cpp"
         "time_sync_protocol.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES uart_comm esp_timer
)
