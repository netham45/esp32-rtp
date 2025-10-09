#include "sleep.h"
#include "lifecycle_internal.h"
#include "../global.h"
#include "../config/config_manager.h"
#include "../receiver/sap_listener.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_timer.h"

// External event group from lifecycle_manager.c
extern EventGroupHandle_t s_network_activity_event_group;

// Forward declarations for external audio tracking variables
extern bool is_silent;
extern uint32_t silence_duration_ms;
extern uint64_t last_audio_time;

// Get lifecycle context and state
extern lifecycle_context_t* lifecycle_get_context(void);
extern lifecycle_state_t lifecycle_get_current_state(void);
extern esp_err_t lifecycle_manager_post_event(lifecycle_event_t event);

// Context access - use local variable instead of macro for write access
static inline lifecycle_context_t* get_ctx(void) {
    return lifecycle_get_context();
}

/**
 * @brief Network monitor task
 * 
 * Monitors network activity during sleep mode and wakes the device
 * when sufficient activity is detected.
 */
static void network_monitor_task(void *params) {
    ESP_LOGI(TAG, "Network monitor task started");
    lifecycle_context_t *ctx = get_ctx();

    // Initialize the last packet time
    ctx->last_packet_time = xTaskGetTickCount();

    while (true) {
        if (ctx->monitoring_active) {
            // Use cached interval value for thread-safe access
            uint32_t check_interval = ctx->cached_network_check_interval_ms;
            
            // Wait for a packet notification OR timeout
            EventBits_t bits = xEventGroupWaitBits(
                s_network_activity_event_group,   // The event group being tested.
                NETWORK_PACKET_RECEIVED_BIT,      // The bits within the event group to wait for.
                pdTRUE,                           // NETWORK_PACKET_RECEIVED_BIT should be cleared before returning.
                pdFALSE,                          // Don't wait for all bits, any bit will do (we only have one).
                pdMS_TO_TICKS(check_interval)     // Use cached value for wait time
            );

            // Check if still monitoring after the wait (could have been disabled by exit_silence_sleep_mode)
            if (!ctx->monitoring_active) {
                continue; // Exit loop iteration if monitoring was stopped during wait
            }

            TickType_t current_time = xTaskGetTickCount();
            TickType_t time_since_last_packet = (current_time - ctx->last_packet_time) * portTICK_PERIOD_MS;

            // Get cached threshold values for thread-safe access
            uint8_t activity_threshold = ctx->cached_activity_threshold_packets;
            uint32_t inactivity_timeout = ctx->cached_network_inactivity_timeout_ms;

            // Did we receive a packet notification?
            if (bits & NETWORK_PACKET_RECEIVED_BIT) {
                ESP_LOGD(TAG, "Monitor: Packet received event bit set.");
                // packet_counter and last_packet_time are updated in network.c when the bit is set
                // Check if the activity threshold is met
                if (ctx->packet_counter >= activity_threshold) {
                    ESP_LOGI(TAG, "Network activity threshold met (%" PRIu32 " packets >= %d), exiting sleep mode",
                            ctx->packet_counter, activity_threshold);
                    lifecycle_manager_post_event(LIFECYCLE_EVENT_WAKE_UP);
                } else {
                    ESP_LOGD(TAG, "Monitor: Packet count %" PRIu32 " < threshold %d", ctx->packet_counter, activity_threshold);
                }
            } else {
                // No packet notification bit set, timeout occurred. Check for inactivity timeout.
                ESP_LOGD(TAG, "Monitor: Wait timeout. Packets=%" PRIu32 ", time_since_last=%lu ms", ctx->packet_counter, (unsigned long)time_since_last_packet);
                if (time_since_last_packet >= inactivity_timeout) {
                    ESP_LOGI(TAG, "Network inactivity timeout reached (%lu ms >= %lu ms), maintaining sleep mode",
                            (unsigned long)time_since_last_packet, (unsigned long)inactivity_timeout);
                    // Update timestamp to prevent continuous logging of the same timeout event
                    // Note: last_packet_time is only updated here on timeout, or in network.c on packet arrival.
                    ctx->last_packet_time = current_time;
                }
            }
            // The loop continues, waiting again with xEventGroupWaitBits which includes the delay

        } else {
            // When not actively monitoring, suspend the task to save CPU
            ESP_LOGD(TAG, "Monitoring inactive, suspending monitor task.");
            // Clear any pending event bits before suspending
            if (s_network_activity_event_group) {
                xEventGroupClearBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
            }
            vTaskSuspend(NULL);
            // --- Task resumes here when vTaskResume is called (in enter_silence_sleep_mode) ---
            ESP_LOGD(TAG, "Monitor task resumed.");
            // Reset state when resuming
            ctx->last_packet_time = xTaskGetTickCount();
            ctx->packet_counter = 0; // Reset packet counter when monitoring starts/resumes
            // Clear event bits again on resume just in case
            if (s_network_activity_event_group) {
                xEventGroupClearBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
            }
        }
    }
}

void lifecycle_sleep_enter_silence_mode(void) {
    app_config_t *config = config_manager_get_config();
    if (config->device_mode != MODE_RECEIVER_USB && config->device_mode != MODE_RECEIVER_SPDIF) {
        ESP_LOGI(TAG, "Device is in sender mode, silence sleep is disabled");
        return;
    }
    
    ESP_LOGI(TAG, "Entering silence sleep mode");
    
    // Stop SAP listener to save power during sleep
    if (sap_listener_is_running()) {
        esp_err_t ret = sap_listener_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop SAP listener for sleep mode: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "SAP listener stopped for sleep mode");
        }
    }
    
    // Make sure event group exists
    if (s_network_activity_event_group == NULL) {
        ESP_LOGE(TAG, "Network activity event group not initialized, cannot enter sleep mode");
        return;
    }
    
    // Update cached values from config for thread-safe access in monitor task
    lifecycle_context_t *ctx = get_ctx();
    ctx->cached_silence_threshold_ms = config->silence_threshold_ms;
    ctx->cached_network_check_interval_ms = config->network_check_interval_ms;
    ctx->cached_activity_threshold_packets = config->activity_threshold_packets;
    ctx->cached_silence_amplitude_threshold = config->silence_amplitude_threshold;
    ctx->cached_network_inactivity_timeout_ms = config->network_inactivity_timeout_ms;
    
    ESP_LOGI(TAG, "Sleep monitoring configured: silence_threshold=%lums, check_interval=%lums, "
             "activity_threshold=%d packets, amplitude_threshold=%d, inactivity_timeout=%lums",
             ctx->cached_silence_threshold_ms, ctx->cached_network_check_interval_ms,
             ctx->cached_activity_threshold_packets, ctx->cached_silence_amplitude_threshold,
             ctx->cached_network_inactivity_timeout_ms);
    
    // Configure WiFi for max power saving
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi power save mode: %s", esp_err_to_name(ret));
    }

    // Suppress WiFi warnings
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    // Create network monitoring task if it doesn't exist yet
    if (ctx->network_monitor_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            network_monitor_task,
            "network_monitor",
            4096,
            NULL,
            5,  // Low priority
            &ctx->network_monitor_task_handle,
            0   // Core 0
        );
        if (task_ret != pdTRUE) {
            ESP_LOGE(TAG, "Failed to create network monitor task");
            return;
        }
    }
    
    // Start network monitoring
    ctx->monitoring_active = true;
    ctx->packet_counter = 0;
    if (eTaskGetState(ctx->network_monitor_task_handle) == eSuspended) {
        vTaskResume(ctx->network_monitor_task_handle);
    }
    
    ESP_LOGI(TAG, "Entered light sleep mode with network monitoring");
}

void lifecycle_sleep_exit_silence_mode(void) {
    ESP_LOGI(TAG, "Exiting silence sleep mode");
    lifecycle_context_t *ctx = get_ctx();
    
    // Stop the network monitoring
    ctx->monitoring_active = false;
    
    // Set WiFi back to normal power saving mode
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi normal mode: %s", esp_err_to_name(ret));
    }

    // Reset silence tracking variables to prevent immediate re-entry into sleep mode
    is_silent = false;
    silence_duration_ms = 0;
    last_audio_time = esp_timer_get_time() / 1000; // Reset to current time
    
    // Restart SAP listener if we're in a receiver mode
    lifecycle_state_t current_state = lifecycle_get_current_state();
    if (current_state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
        current_state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
        if (!sap_listener_is_running()) {
            ret = sap_listener_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restart SAP listener after sleep: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "SAP listener restarted after sleep");
            }
        }
    }
    
    ESP_LOGI(TAG, "Resumed normal operation");
}

void lifecycle_sleep_report_network_activity(void) {
    lifecycle_context_t *ctx = get_ctx();
    if (ctx->monitoring_active) {
        ctx->packet_counter++;
        ctx->last_packet_time = xTaskGetTickCount();
        // Signal the network monitor task that a packet has been received
        if (s_network_activity_event_group) {
            xEventGroupSetBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
        }
    }
}

void lifecycle_sleep_update_params(void) {
    app_config_t *config = config_manager_get_config();
    lifecycle_context_t *ctx = get_ctx();
    
    // Update cached values if monitoring is active
    if (ctx->monitoring_active) {
        ctx->cached_silence_threshold_ms = config->silence_threshold_ms;
        ctx->cached_network_check_interval_ms = config->network_check_interval_ms;
        ctx->cached_activity_threshold_packets = config->activity_threshold_packets;
        ctx->cached_silence_amplitude_threshold = config->silence_amplitude_threshold;
        ctx->cached_network_inactivity_timeout_ms = config->network_inactivity_timeout_ms;
        
        ESP_LOGI(TAG, "Updated active sleep monitoring parameters");
    }
}