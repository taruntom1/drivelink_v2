idf_component_register(
    SRCS "src/motor_manager.cpp"
         "src/motor_manager_uart.cpp"
         "src/motor_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES motor_controller uart_comm rtos_util freertos log esp_timer protocol
    PRIV_REQUIRES diag
)
