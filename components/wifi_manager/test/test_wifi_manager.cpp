#include "WifiManager.hpp"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

TEST_CASE("WifiManager: freshly constructed object reports disconnected", "[wifi_manager]") {
    WifiManager wifi;
    TEST_ASSERT_FALSE(wifi.isConnected());
    TEST_ASSERT_EQUAL_INT8(0, wifi.getRssi());
    TEST_ASSERT_NOT_NULL(wifi.eventGroup());
}

// Brings up the real Wi-Fi driver, netifs and NVS — on-target only.
//
// `[leaks]` turns off the runner's blanket leak check so this case can make a
// sharper assertion of its own. The runner measures the whole case against one
// threshold, which cannot separate ESP-IDF's one-time TCP/IP initialisation
// from a genuine per-cycle leak; the check below does exactly that.
TEST_CASE("WifiManager RAII: repeated construct/init/destroy costs nothing",
          "[wifi_manager][hw][leaks]") {
    // The first cycle after a boot also pays for starting lwIP. esp_netif_init()
    // brings the stack up and esp_netif_deinit() then refuses to take it down —
    // "deinit of LwIP not supported", esp_netif/lwip/esp_netif_lwip.c, which
    // returns ESP_ERR_NOT_SUPPORTED. That cost (measured at 5,684 bytes) is the
    // platform's, not this class's, so it is paid here and left out of the
    // window below.
    {
        WifiManager warmup;
        TEST_ESP_OK(warmup.init());
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // let the idle task reclaim driver stacks

    // What RAII does have to guarantee: once the platform is up, a cycle
    // returns everything it took, however many times it runs. Before the
    // destructor released the default-wifi driver handlers, the default event
    // loop and NVS, this leaked 11,428 bytes on the first cycle — and every
    // init() after it failed with ESP_ERR_INVALID_STATE, because
    // esp_event_loop_create_default() kept finding a loop nobody had deleted.
    const size_t before = esp_get_free_heap_size();

    constexpr int kCycles = 3;
    for (int cycle = 0; cycle < kCycles; cycle++) {
        WifiManager wifi;
        TEST_ESP_OK(wifi.init());  // must keep succeeding, not just keep quiet
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    const size_t after = esp_get_free_heap_size();

    // Three cycles, so any per-cycle leak is tripled before it is measured —
    // the old 11,428-byte one would show as ~34 KB against this 1 KB band,
    // which absorbs only allocator fragmentation.
    TEST_ASSERT_INT_WITHIN(1024, static_cast<int>(before), static_cast<int>(after));
}
