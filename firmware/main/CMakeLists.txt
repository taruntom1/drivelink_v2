idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES uart_comm mod_imu_bno055 diag core core_os protocol mod_mock mod_motor)
