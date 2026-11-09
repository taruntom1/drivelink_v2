idf_component_register(
    SRCS "src/stoppable_task.cpp"
         "src/task_timer.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_common esp_timer
    PRIV_REQUIRES log
)
