idf_component_register(
    SRCS "src/state.cpp" "src/driver.cpp" "src/module.cpp" "src/idf/bus_idf.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core core_os protocol
    PRIV_REQUIRES esp_driver_i2c rtos_util freertos)
