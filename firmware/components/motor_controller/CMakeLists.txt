idf_component_register(
    SRCS "src/motor_controller.cpp" "src/velocity_estimator.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mod_motor motor_hal rotary_encoder_hal pid freertos log
)
