# core_os: thin OS adapters for the pure core. The API headers in include/
# are shared; the backend is picked by include dir and sources, never by
# #ifdef. This file builds the ESP-IDF backend only. The native backend
# (backend/native/) is built by firmware/test/native/CMakeLists.txt.
idf_component_register(
    SRCS "backend/idf/src/core_os_idf.cpp"
         "backend/idf/src/periodic_task_idf.cpp"
    INCLUDE_DIRS "include" "backend/idf/include"
    REQUIRES freertos rtos_util protocol
    PRIV_REQUIRES diag esp_timer log
)
