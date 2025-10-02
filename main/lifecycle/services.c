#include "services.h"
#include "lifecycle_internal.h"
#include "../global.h"
#include "../config/config_manager.h"
#include "../wifi/wifi_manager.h"
#include "../web/web_server.h"
#include "../mdns/mdns_discovery.h"
#include "../mdns/mdns_service.h"
#include "../ntp/ntp_client.h"
#ifdef IS_SPDIF
#include "spdif_in.h"
#endif
#include "esp_log.h"

esp_err_t lifecycle_services_init_wifi(void) {
    // Initialize WiFi FIRST to allocate GDMA channels for crypto before RMT claims them
    ESP_LOGI(TAG, "Initializing WiFi manager...");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
        return ret;
    }

    // Try to connect to the strongest network first
    ESP_LOGI(TAG, "Attempting to connect to strongest WiFi network...");
    ret = wifi_manager_connect_to_strongest();

    // If connecting to strongest network fails, fall back to normal behavior:
    // 1. Connect using stored credentials if available
    // 2. Start AP mode with captive portal if no credentials are stored
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Could not connect to strongest network, falling back to stored credentials or AP mode");
        ret = wifi_manager_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi manager: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t lifecycle_services_init_web_server(void) {
    // Start the web server (works in both AP mode and STA mode)
    ESP_LOGI(TAG, "Starting web server for configuration...");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    web_server_start();
    return ESP_OK;
}

esp_err_t lifecycle_services_init_spdif_receiver(void) {
    #ifdef IS_SPDIF
    app_config_t *config = config_manager_get_config();
    if (config->device_mode == MODE_SENDER_SPDIF) {
        ESP_LOGI(TAG, "Initializing S/PDIF receiver with pin %d", config->spdif_data_pin);
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        esp_err_t ret = spdif_receiver_init(6, NULL); // Use hardcoded pin from original implementation
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize S/PDIF receiver: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    #endif
    return ESP_OK;
}

esp_err_t lifecycle_services_init_mdns(void) {
    app_config_t *config = config_manager_get_config();

    // Initialize mDNS service first (required for both discovery and advertisement)
    ESP_LOGI(TAG, "Initializing mDNS service");
    esp_err_t ret = mdns_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mDNS service: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start mDNS service advertisement
    ESP_LOGI(TAG, "Starting mDNS service advertisement");
    mdns_service_start();

    // Start mDNS discovery if enabled
    if (config->enable_mdns_discovery) {
        ESP_LOGI(TAG, "Starting mDNS discovery");
        ret = mdns_discovery_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start mDNS discovery: %s", esp_err_to_name(ret));
            // Don't fail initialization if discovery fails - advertisement still works
        }
    }

    // Start NTP client
    // initialize_ntp_client(); // DISABLED: causing socket leak (errno 112)

    return ESP_OK;
}