#include "WifiManager.hpp"
#include "unity.h"

TEST_CASE("WifiManager: freshly constructed object reports disconnected", "[wifi_manager]") {
    WifiManager wifi;
    TEST_ASSERT_FALSE(wifi.isConnected());
    TEST_ASSERT_EQUAL_INT8(0, wifi.getRssi());
    TEST_ASSERT_NOT_NULL(wifi.eventGroup());
}

// Brings up the real Wi-Fi driver and NVS — on-target only.
TEST_CASE("WifiManager RAII: construct/init/destroy releases driver resources cleanly",
          "[wifi_manager][hw]") {
    WifiManager wifi;
    TEST_ESP_OK(wifi.init());
}  // destructor stops and deinits the Wi-Fi driver here
