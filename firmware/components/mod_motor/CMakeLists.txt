idf_component_register(SRCS "src/controller.cpp" "src/velocity_estimator.cpp"
                            "src/pwm_dir.cpp" "src/quadrature.cpp" "src/hardware_types.cpp"
                            "src/idf/motor_hw_idf.cpp"
    INCLUDE_DIRS "include" REQUIRES core core_os protocol pid
    PRIV_REQUIRES motor_hal rotary_encoder_hal)
