idf_component_register(
    SRCS "src/HealthMonitor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi freertos
    PRIV_REQUIRES common esp_timer
)
