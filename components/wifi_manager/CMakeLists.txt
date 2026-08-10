idf_component_register(
    SRCS "src/WifiManager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer esp_http_server
    EMBED_FILES "provisioning.html"
)
