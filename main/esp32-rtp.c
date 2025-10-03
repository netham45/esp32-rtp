#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP system headers
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// Project configuration
#include "config.h"

// Module headers
#include "config/config_manager.h"
#include "wifi/wifi_manager.h"
#include "lifecycle_manager.h"
#include "logging/log_buffer.h"
#include "bq25895_integration.h"
#include "TS3USB30ERSWR/usb_switch.h"


// Function to check if a GPIO pin is pressed (connected to ground)
bool is_gpio_pressed(gpio_num_t pin) {
    return gpio_get_level(pin) == 0; // Returns true if pin is low (pressed)
}


void app_main(void)
{
    // Initialize NVS (required for USB subsystem)
    BaseType_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize log buffer system
    ESP_LOGI(TAG, "Initializing log buffer system");
    esp_err_t log_err = log_buffer_init();
    if (log_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize log buffer: %s", esp_err_to_name(log_err));
    } else {
        ESP_LOGI(TAG, "Log buffer initialized successfully");
    }

    // Initialize BQ25895 Battery Charger
    ESP_LOGI(TAG, "Initializing BQ25895 battery charger");
    esp_err_t bq_err = bq25895_integration_init();
    if (bq_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BQ25895: %s", esp_err_to_name(bq_err));
        // Decide if this is a critical failure or if the app can continue
    } else {
        ESP_LOGI(TAG, "BQ25895 initialized successfully");
    }
    
    // Initialize USB switch
    ESP_LOGI(TAG, "Initializing USB switch");
    esp_err_t usb_err = usb_switch_init();
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB switch: %s", esp_err_to_name(usb_err));
        // Non-critical failure, continue
    } else {
        ESP_LOGI(TAG, "USB switch initialized successfully");
    }
    
    // Get the wake cause (why the device booted)
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    
    // Only check GPIO pins for reset if this was a power-on or hard reset, not waking from sleep
    if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // This is a power-on or hard reset, not a wake from sleep
        
        // Configure GPIO pins 0 and 1 as inputs with pull-up resistors
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_1),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        
        // Wait 3 seconds and check if pins 0 or 1 are pressed
        ESP_LOGI(TAG, "Starting 3-second WiFi reset window. Press GPIO 0 or 1 to reset WiFi config...");
        for (int i = 0; i < 30; i++) { // 30 * 100ms = 3 seconds
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // Check if either pin is pressed
            if (is_gpio_pressed(GPIO_NUM_0) || is_gpio_pressed(GPIO_NUM_1)) {
                ESP_LOGI(TAG, "GPIO pin pressed! Wiping WiFi configuration...");
                
                // Initialize WiFi manager if it's not already initialized
                wifi_manager_init();
                
                // Clear WiFi credentials from NVS
                wifi_manager_clear_credentials();
                
                // Clear WiFi credentials from ESP's internal WiFi storage
                esp_wifi_restore();
                
                // Reset all settings to defaults
                config_manager_reset();
                
                // To be extra safe, erase the entire NVS (all namespaces)
                nvs_flash_erase();
                
                ESP_LOGI(TAG, "All settings reset to defaults. Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second for logs to be printed
                esp_restart(); // Restart the ESP32
            }
        }
        ESP_LOGI(TAG, "WiFi reset window closed. Continuing with normal startup...");
    } else {
        // This is a wake from sleep, skip the GPIO reset check
        ESP_LOGI(TAG, "Waking from sleep (cause: %d), skipping WiFi reset window", wakeup_cause);
    }
    
    // Start the lifecycle manager (which will initialize config_manager)
    ESP_LOGI(TAG, "Starting lifecycle manager");
    ESP_ERROR_CHECK(lifecycle_manager_init());
    
    // The main loop is now handled by the lifecycle manager.
    // This task can now exit.
}
