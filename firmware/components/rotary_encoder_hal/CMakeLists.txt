idf_component_register(
    SRCS "src/encoder.cpp"
         "src/pcnt_pool.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_pcnt esp_driver_gpio
)
