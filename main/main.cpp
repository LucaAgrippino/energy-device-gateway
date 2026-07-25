#include "esp_log.h"

namespace {
constexpr const char* kTag = "main";
}

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "energy-device-gateway starting");
}
