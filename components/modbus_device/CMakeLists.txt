idf_component_register(
    SRCS "src/ModbusRtuDevice.cpp" "src/ModbusRtuTask.cpp"
         "src/ModbusTcpDevice.cpp" "src/ModbusTcpTask.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos common
    PRIV_REQUIRES esp_timer lwip
)
