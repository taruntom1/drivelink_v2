idf_component_register(SRCS "src/controller.cpp" "src/velocity_estimator.cpp"
    INCLUDE_DIRS "include" REQUIRES core core_os protocol pid)
