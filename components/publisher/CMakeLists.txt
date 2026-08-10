idf_component_register(
    SRCS "src/WsPublisher.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server common
    EMBED_FILES "../../dashboard/index.html"
)
