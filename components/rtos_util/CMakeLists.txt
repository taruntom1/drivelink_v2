idf_component_register(
    SRCS "src/stoppable_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_common
    PRIV_REQUIRES log
)
