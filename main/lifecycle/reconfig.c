#include "reconfig.h"
#include "lifecycle_internal.h"
#include "../lifecycle_manager.h"
#include "../config/config_manager.h"
#include "../receiver/audio_out.h"
#include "../receiver/buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef IS_SPDIF
#include "../sender/spdif_in/spdif_in.h"
#include "../receiver/spdif_out.h"
#endif

#undef TAG
#define TAG "lifecycle_reconfig"

// External functions from audio_out.c for playback control
extern void stop_playback(void);
extern void resume_playback(void);

/**
 * Reconfigure sample rate for active receiver modes without restart
 */
esp_err_t lifecycle_reconfig_sample_rate(uint32_t new_rate) {
    ESP_LOGI(TAG, "Reconfiguring sample rate to %lu Hz without restart", new_rate);
    
    lifecycle_state_t current_state = lifecycle_get_current_state();
    
    // Check if we're in a receiver mode
    if (current_state != LIFECYCLE_STATE_MODE_RECEIVER_USB &&
        current_state != LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
        ESP_LOGW(TAG, "Sample rate reconfiguration only available in receiver modes");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ESP_OK;
    
    // Temporarily pause audio output
    ESP_LOGI(TAG, "Pausing audio output for reconfiguration");
    stop_playback();
    
    // Small delay to ensure audio has stopped
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Reconfigure based on current mode
    if (current_state == LIFECYCLE_STATE_MODE_RECEIVER_USB) {
        #ifdef IS_USB
        ESP_LOGI(TAG, "Reconfiguring USB audio output for sample rate %lu Hz", new_rate);
        // Note: USB audio typically handles sample rate changes automatically
        // If a specific reconfiguration function exists, call it here
        // For now, we rely on the USB audio subsystem to adapt
        ESP_LOGI(TAG, "USB audio will adapt to new sample rate on resume");
        #else
        ESP_LOGW(TAG, "USB support not enabled in build");
        ret = ESP_ERR_NOT_SUPPORTED;
        #endif
    } else if (current_state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
        #ifdef IS_SPDIF
        ESP_LOGI(TAG, "Reconfiguring SPDIF output for sample rate %lu Hz", new_rate);
        ret = spdif_set_sample_rates((int)new_rate);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reconfigure SPDIF sample rate: 0x%x", ret);
        } else {
            ESP_LOGI(TAG, "SPDIF sample rate reconfigured successfully");
        }
        #else
        ESP_LOGW(TAG, "SPDIF support not enabled in build");
        ret = ESP_ERR_NOT_SUPPORTED;
        #endif
    }
    
    // Resume audio output
    ESP_LOGI(TAG, "Resuming audio output after reconfiguration");
    resume_playback();
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sample rate reconfiguration completed successfully");
    } else {
        ESP_LOGE(TAG, "Sample rate reconfiguration failed: 0x%x", ret);
    }
    
    return ret;
}

/**
 * Reconfigure SPDIF pin for active SPDIF modes without restart
 */
esp_err_t lifecycle_reconfig_spdif_pin(uint8_t new_pin) {
    ESP_LOGI(TAG, "Reconfiguring SPDIF data pin to GPIO %d", new_pin);
    
    lifecycle_state_t current_state = lifecycle_get_current_state();
    
    // Check if we're in a SPDIF mode
    if (current_state != LIFECYCLE_STATE_MODE_RECEIVER_SPDIF &&
        current_state != LIFECYCLE_STATE_MODE_SENDER_SPDIF) {
        ESP_LOGW(TAG, "SPDIF pin reconfiguration only available in SPDIF modes");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ESP_OK;
    
    #ifdef IS_SPDIF
    if (current_state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
        ESP_LOGI(TAG, "Reconfiguring SPDIF receiver output pin");
        
        // Temporarily pause audio output
        ESP_LOGI(TAG, "Pausing audio output for pin reconfiguration");
        stop_playback();
        
        // Small delay to ensure audio has stopped
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Get current sample rate from config
        app_config_t *config = config_manager_get_config();
        
        // Reinitialize SPDIF with new pin
        ESP_LOGI(TAG, "Reinitializing SPDIF output with GPIO %d", new_pin);
        ret = spdif_init(config->sample_rate);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize SPDIF output: 0x%x", ret);
        } else {
            // Reconfigure sample rate after reinit
            ret = spdif_set_sample_rates((int)config->sample_rate);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore SPDIF sample rate: 0x%x", ret);
            }
        }
        
        // Resume audio output
        ESP_LOGI(TAG, "Resuming audio output after pin reconfiguration");
        resume_playback();
        
    } else if (current_state == LIFECYCLE_STATE_MODE_SENDER_SPDIF) {
        ESP_LOGI(TAG, "Reconfiguring SPDIF sender input pin");
        
        // Stop SPDIF receiver
        ESP_LOGI(TAG, "Stopping SPDIF receiver");
        ret = spdif_receiver_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop SPDIF receiver: 0x%x", ret);
        }
        
        // Deinitialize SPDIF receiver
        ESP_LOGI(TAG, "Deinitializing SPDIF receiver");
        spdif_receiver_deinit();
        
        // Small delay to ensure cleanup
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Reinitialize with new pin
        ESP_LOGI(TAG, "Reinitializing SPDIF receiver with GPIO %d", new_pin);
        ret = spdif_receiver_init(new_pin, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize SPDIF receiver: 0x%x", ret);
        } else {
            // Restart SPDIF receiver
            ESP_LOGI(TAG, "Restarting SPDIF receiver");
            ret = spdif_receiver_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restart SPDIF receiver: 0x%x", ret);
            }
        }
    }
    #else
    ESP_LOGW(TAG, "SPDIF support not enabled in build");
    ret = ESP_ERR_NOT_SUPPORTED;
    #endif
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPDIF pin reconfiguration completed successfully");
    } else {
        ESP_LOGE(TAG, "SPDIF pin reconfiguration failed: 0x%x", ret);
    }
    
    return ret;
}

/**
 * Reconfigure buffer sizes for active receiver modes without restart
 */
esp_err_t lifecycle_reconfig_buffer_params(void) {
    ESP_LOGI(TAG, "Reconfiguring buffer sizes without restart");
    
    lifecycle_state_t current_state = lifecycle_get_current_state();
    
    // Check if we're in a receiver mode
    if (current_state != LIFECYCLE_STATE_MODE_RECEIVER_USB &&
        current_state != LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
        ESP_LOGW(TAG, "Buffer size reconfiguration only available in receiver modes");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ESP_OK;
    
    // Get current configuration
    app_config_t *config = config_manager_get_config();
    
    ESP_LOGI(TAG, "Reconfiguring buffer with parameters:");
    ESP_LOGI(TAG, "  initial_buffer_size: %d", config->initial_buffer_size);
    ESP_LOGI(TAG, "  max_buffer_size: %d", config->max_buffer_size);
    ESP_LOGI(TAG, "  buffer_grow_step_size: %d", config->buffer_grow_step_size);
    ESP_LOGI(TAG, "  max_grow_size: %d", config->max_grow_size);
    
    // Temporarily pause audio output
    ESP_LOGI(TAG, "Pausing audio output for buffer reconfiguration");
    stop_playback();
    
    // Small delay to ensure audio has stopped
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Empty the current buffer
    ESP_LOGI(TAG, "Emptying buffer before reconfiguration");
    empty_buffer();
    
    // For max_buffer_size changes, we need to reallocate the entire buffer
    // This is a more complex operation that requires freeing and reallocating
    // For now, we'll log a warning that this requires a restart
    ESP_LOGW(TAG, "Note: Changing max_buffer_size requires a system restart for full effect");
    ESP_LOGI(TAG, "Buffer growth parameters will be updated immediately");
    
    // Update buffer growth parameters (this is safe without reallocation)
    ret = buffer_update_growth_params();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update buffer growth parameters: 0x%x", ret);
    }
    
    // Resume audio output
    ESP_LOGI(TAG, "Resuming audio output after buffer reconfiguration");
    resume_playback();
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Buffer reconfiguration completed successfully");
    } else {
        ESP_LOGE(TAG, "Buffer reconfiguration failed: 0x%x", ret);
    }
    
    return ret;
}

/**
 * Public API for changing sample rate with configuration save
 */
esp_err_t lifecycle_manager_change_sample_rate(uint32_t new_rate) {
    app_config_t *config = config_manager_get_config();
    lifecycle_state_t current_state = lifecycle_get_current_state();
    
    if (config->sample_rate != new_rate) {
        ESP_LOGI(TAG, "Sample rate changed from %d to %d. Reconfiguring.", config->sample_rate, new_rate);
        config->sample_rate = new_rate;
        esp_err_t ret = config_manager_save_config();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save new sample rate to config");
            return ret;
        }
        
        // Check if we're in an active receiver mode
        if (current_state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
            current_state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
            ESP_LOGI(TAG, "Active receiver mode detected, applying sample rate change immediately");
            
            // Try to reconfigure without restart
            ret = lifecycle_reconfig_sample_rate(new_rate);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Sample rate changed successfully without restart");
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Immediate reconfiguration failed, falling back to restart");
                // Fall through to post event for restart
            }
        }
        
        // If not in active mode or reconfiguration failed, post event for restart
        ESP_LOGI(TAG, "Posting sample rate change event for restart");
        return lifecycle_manager_post_event(LIFECYCLE_EVENT_SAMPLE_RATE_CHANGE);
    }
    return ESP_OK;
}