idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES motor_manager uart_comm time_sync imu_bmo_055 diag core core_os protocol mod_mock mod_motor)
