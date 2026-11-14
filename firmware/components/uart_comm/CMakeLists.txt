idf_component_register(
    SRCS "src/uart_comm.cpp"
         "src/parser.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_driver_uart
    PRIV_REQUIRES protocol rtos_util esp_timer
)
