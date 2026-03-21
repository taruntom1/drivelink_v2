idf_component_register(
    SRCS "src/motor_manager.cpp"
         "src/motor_manager_uart.cpp"
    INCLUDE_DIRS "include"
    REQUIRES motor_controller uart_comm freertos log
)
