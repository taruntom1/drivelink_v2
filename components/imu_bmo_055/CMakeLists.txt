idf_component_register(
    SRCS "vendor/bno055.c"
         "src/bno055_device.cpp"
         "src/imu_bmo_055.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include" "vendor"
    REQUIRES uart_comm rtos_util driver freertos log esp_timer
)
