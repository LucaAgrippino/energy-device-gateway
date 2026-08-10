idf_component_register(
    SRCS "src/Mpu9150.cpp" "src/ImuTask.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
    PRIV_REQUIRES common
)
