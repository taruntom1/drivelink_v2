idf_component_register(
    SRCS "src/esp_motor.cpp"
         "src/mcpwm_pool.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_mcpwm esp_driver_gpio
)
