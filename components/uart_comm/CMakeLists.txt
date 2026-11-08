idf_component_register(
    SRCS "src/uart_comm.cpp"
         "src/frame.cpp"
         "src/parser.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_driver_uart
    PRIV_REQUIRES rtos_util
)
