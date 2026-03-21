idf_component_register(
    SRCS "src/motor_controller.cpp"
    INCLUDE_DIRS "include"
    REQUIRES motor_hal rotary_encoder_hal pid freertos log
)
