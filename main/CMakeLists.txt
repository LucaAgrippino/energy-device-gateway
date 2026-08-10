idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver imu modbus_device wifi_manager aggregator publisher common
)
