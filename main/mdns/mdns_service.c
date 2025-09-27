#include "mdns_service.h"
#include "mdns_discovery.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MDNS_SERVICE";

/**
 * @brief Initializes and starts the mDNS service.
 * 
 * This function now acts as a wrapper around the unified mDNS discovery
 * implementation. It enables advertisement and starts the discovery service.
 */
 void mdns_service_start(void) {
     ESP_LOGI(TAG, "==== MDNS_SERVICE_START CALLED ====");
     
     ESP_LOGI(TAG, "Starting mDNS service (using unified implementation)");
     ESP_LOGI(TAG, ">>> Calling mdns_discovery_start() <<<");
     
     // Start the mDNS discovery service (which now handles both initialization and advertisement)
     // Advertisement is automatically enabled after network is ready in mdns_discovery_start()
     esp_err_t err = mdns_discovery_start();
     
     if (err == ESP_OK) {
         ESP_LOGI(TAG, "mdns_discovery_start() returned ESP_OK");
         ESP_LOGI(TAG, "==== MDNS SERVICE STARTED SUCCESSFULLY ====");
         ESP_LOGI(TAG, "Advertisement will be enabled automatically after network stabilization");
     } else {
         ESP_LOGE(TAG, "FAILED to start mDNS service: %s (0x%x)", esp_err_to_name(err), err);
     }
 }

/**
 * @brief Stops the mDNS service.
 *
 * This function disables advertisement and stops the mDNS discovery service.
 */
void mdns_service_stop(void) {
    ESP_LOGI(TAG, "Stopping mDNS service");
    
    
    // Disable advertisement first
    mdns_discovery_enable_advertisement(false);
    
    // Stop the mDNS discovery service
    esp_err_t err = mdns_discovery_stop();
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mDNS service stopped successfully");
    } else {
        ESP_LOGE(TAG, "Error stopping mDNS service: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Updates the mDNS hostname without requiring a reboot
 *
 * This function stops the current mDNS service, updates the hostname,
 * and restarts the service with the new hostname.
 *
 * @return ESP_OK on success, or appropriate error code on failure
 */
esp_err_t mdns_service_update_hostname(void) {
    ESP_LOGI(TAG, "Updating mDNS hostname");
    
    // Stop the current advertisement
    ESP_LOGI(TAG, "Stopping current mDNS advertisement");
    mdns_discovery_enable_advertisement(false);
    
    // Small delay to ensure service is properly removed
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // The hostname will be regenerated from lifecycle configuration
    // when advertisement is re-enabled
    
    // Re-enable advertisement with new hostname
    ESP_LOGI(TAG, "Re-enabling mDNS advertisement with new hostname");
    mdns_discovery_enable_advertisement(true);
    
    ESP_LOGI(TAG, "mDNS hostname updated successfully");
    return ESP_OK;
}
