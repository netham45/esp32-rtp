#include "hw_init.h"
#include "lifecycle_internal.h"
#include "../global.h"
#include "../config/config_manager.h"
#include "../ota/ota_manager.h"
#include "bq25895_integration.h"
#include "nvs_flash.h"
#include "esp_pm.h"
#include "esp_log.h"

esp_err_t lifecycle_hw_init_nvs(void) {
    ESP_LOGI(TAG, "Initializing NVS");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t lifecycle_hw_init_config(void) {
    ESP_LOGI(TAG, "Initializing configuration manager");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    return config_manager_init();
}

esp_err_t lifecycle_hw_init_ota(void) {
    ESP_LOGI(TAG, "Initializing OTA manager");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t ota_err = ota_manager_init(NULL);  // Use default config
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize OTA manager: %s", esp_err_to_name(ota_err));
        // Non-critical, continue
        return ota_err;
    }
    
    ESP_LOGI(TAG, "OTA manager initialized successfully");
    
    // Mark current firmware as valid to prevent rollback
    // This must be done after every successful boot with new firmware
    esp_err_t mark_err = ota_manager_mark_valid();
    if (mark_err == ESP_OK) {
        ESP_LOGI(TAG, "Current firmware marked as valid, rollback protection active");
    } else {
        ESP_LOGW(TAG, "Failed to mark firmware as valid: 0x%x", mark_err);
    }
    
    // Clear any leftover OTA state from previous update
    // This ensures clean state for the next OTA attempt
    ota_manager_clear_status();
    ESP_LOGI(TAG, "OTA status cleared, ready for new updates");
    
    return ESP_OK;
}

esp_err_t lifecycle_hw_init_battery(void) {
    ESP_LOGI(TAG, "Initializing BQ25895 battery charger");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t bq_err = bq25895_integration_init();
    if (bq_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BQ25895: %s", esp_err_to_name(bq_err));
        // Non-critical, continue
        return bq_err;
    }
    
    ESP_LOGI(TAG, "BQ25895 initialized successfully");
    return ESP_OK;
}

esp_err_t lifecycle_hw_init_power_management(void) {
    #if CONFIG_PM_ENABLE
    ESP_LOGI(TAG, "Configuring power management (reduced CPU clock)");
    #if CONFIG_IDF_TARGET_ESP32
    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
    #if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
    #else
        .light_sleep_enable = false
    #endif
    };
    #elif CONFIG_IDF_TARGET_ESP32S3
    esp_pm_config_esp32s3_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
    #if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
    #else
        .light_sleep_enable = false
    #endif
    };
    #endif
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Power management not supported or not enabled in menuconfig");
        return err;
    }
    return err;
    #else
    ESP_LOGW(TAG, "Power management not enabled in menuconfig");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

esp_err_t lifecycle_hw_init_dac_detection(void) {
    app_config_t *config = config_manager_get_config();
    
    // Log configuration at initialization
    ESP_LOGI(TAG, "========== CONFIGURATION AT HW_INIT ==========");
    ESP_LOGI(TAG, "device_mode: %d", config->device_mode);
    ESP_LOGI(TAG, "sample_rate: %d", config->sample_rate);
    ESP_LOGI(TAG, "spdif_data_pin: %d", config->spdif_data_pin);
    ESP_LOGI(TAG, "===============================================");

    // If in sender mode, skip DAC detection
    if (config->device_mode == MODE_SENDER_SPDIF) {
        ESP_LOGI(TAG, "S/PDIF sender mode configured, skipping DAC detection");
        return ESP_OK;
    }

    // LAZY USB INITIALIZATION: Skip DAC detection to save memory
    // USB host will be initialized only when actually needed in USB receiver mode
    // This saves ~30KB of heap memory for buffer allocation
    
    if (config->device_mode == MODE_SENDER_USB) {
        ESP_LOGI(TAG, "USB sender mode configured, skipping DAC detection");
    } else if (config->device_mode == MODE_RECEIVER_USB) {
        ESP_LOGI(TAG, "USB receiver mode configured, deferring USB initialization to mode entry");
        // USB will be initialized when entering LIFECYCLE_STATE_MODE_RECEIVER_USB
    } else {
        ESP_LOGI(TAG, "Non-USB mode configured, no USB initialization needed");
    }
    
    // Note: Deep sleep for DAC detection is disabled with lazy initialization
    // If deep sleep behavior is still desired, implement a lightweight USB detection
    // method that doesn't require full USB host initialization

    return ESP_OK;
}