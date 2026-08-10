idf_component_register(
    SRCS "src/Aggregator.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos common
    PRIV_REQUIRES esp_timer
)
