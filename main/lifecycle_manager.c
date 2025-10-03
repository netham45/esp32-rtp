#include "lifecycle_manager.h"
#include "lifecycle/lifecycle_internal.h"
#include "lifecycle/state_machine.h"
#include "lifecycle/reconfig.h"
#include "lifecycle/sap.h"
#include "lifecycle/sleep.h"
#include "lifecycle/config.h"
#include "bq25895_integration.h"
#include "bq25895.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "global.h"
#include "config/config_manager.h"
#include "receiver/network_in.h"
#include "sender/network_out.h"
#include "receiver/audio_out.h"
#include "mdns/mdns_discovery.h"
#include "mdns/mdns_service.h"
#include "esp_wifi.h"

// Lifecycle context (replaces individual static variables)
static lifecycle_context_t s_lifecycle_context = {
    .network_activity_event_group = NULL,
    .network_monitor_task_handle = NULL,
    .packet_counter = 0,
    .monitoring_active = false,
    .last_packet_time = 0,
    .cached_silence_threshold_ms = SILENCE_THRESHOLD_MS,
    .cached_network_check_interval_ms = NETWORK_CHECK_INTERVAL_MS,
    .cached_activity_threshold_packets = ACTIVITY_THRESHOLD_PACKETS,
    .cached_silence_amplitude_threshold = SILENCE_AMPLITUDE_THRESHOLD,
    .cached_network_inactivity_timeout_ms = NETWORK_INACTIVITY_TIMEOUT_MS,
    .current_state = LIFECYCLE_STATE_INITIALIZING
};

// Legacy exports for backward compatibility
EventGroupHandle_t s_network_activity_event_group = NULL;

// Convenience macros for accessing context members
#define monitoring_active (s_lifecycle_context.monitoring_active)
#define cached_silence_threshold_ms (s_lifecycle_context.cached_silence_threshold_ms)
#define cached_network_check_interval_ms (s_lifecycle_context.cached_network_check_interval_ms)
#define cached_activity_threshold_packets (s_lifecycle_context.cached_activity_threshold_packets)
#define cached_silence_amplitude_threshold (s_lifecycle_context.cached_silence_amplitude_threshold)
#define cached_network_inactivity_timeout_ms (s_lifecycle_context.cached_network_inactivity_timeout_ms)

// Context getter functions for submodules
lifecycle_context_t* lifecycle_get_context(void) {
    return &s_lifecycle_context;
}

lifecycle_state_t lifecycle_get_current_state(void) {
    return lifecycle_state_machine_get_current_state();
}

EventGroupHandle_t lifecycle_get_network_event_group(void) {
    return s_network_activity_event_group;
}

/**
 * Public API implementations - thin facades to modules
 */

esp_err_t lifecycle_manager_init(void) {
    // Create the network activity event group if it doesn't exist
    if (s_network_activity_event_group == NULL) {
        s_network_activity_event_group = xEventGroupCreate();
        if (s_network_activity_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create network activity event group");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created network activity event group");
    }

    // Delegate to state machine initialization
    return lifecycle_state_machine_init();
}

esp_err_t lifecycle_manager_post_event(lifecycle_event_t event) {
    // Delegate to state machine
    return lifecycle_state_machine_post_event(event);
}

// lifecycle_manager_change_sample_rate is implemented in lifecycle/reconfig.c

void lifecycle_manager_report_network_activity(void) {
    lifecycle_sleep_report_network_activity();
}

// All configuration getter/setter functions are implemented in lifecycle/config.c

// SAP notification is delegated to lifecycle/sap.c
// esp_err_t lifecycle_manager_notify_sap_stream(...) is in sap.c

esp_err_t lifecycle_get_battery_status(bq25895_status_t *status) {
    return bq25895_integration_get_status(status);
}

esp_err_t lifecycle_get_battery_params(bq25895_charge_params_t *params) {
    return bq25895_integration_get_charge_params(params);
}

esp_err_t lifecycle_set_battery_params(const bq25895_charge_params_t *params) {
    return bq25895_integration_set_charge_params(params);
}

esp_err_t lifecycle_reset_battery(void) {
    return bq25895_integration_reset();
}

esp_err_t lifecycle_set_battery_ce_pin(bool enable) {
    return bq25895_integration_set_ce_pin(enable);
}

esp_err_t lifecycle_read_battery_register(uint8_t reg, uint8_t *value) {
    return bq25895_integration_read_register(reg, value);
}

esp_err_t lifecycle_write_battery_register(uint8_t reg, uint8_t value) {
    return bq25895_integration_write_register(reg, value);
}