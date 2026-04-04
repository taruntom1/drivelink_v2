idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES motor_manager uart_comm time_sync imu_bmo_055)
