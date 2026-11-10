idf_component_register(
    SRCS "vendor/bno055.c"
         "src/bno055_device.cpp"
         "src/imu_bmo_055.cpp"
         "src/imu_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include" "vendor"
    REQUIRES uart_comm rtos_util driver freertos log esp_timer
    PRIV_REQUIRES protocol diag
)
