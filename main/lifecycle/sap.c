#include "sap.h"
#include "lifecycle_internal.h"
#include "../global.h"
#include "../config/config_manager.h"
#include "../receiver/network_in.h"
#include "../receiver/sap_listener.h"
#include "../lifecycle_manager.h"
#include "esp_log.h"
#include <string.h>

esp_err_t lifecycle_manager_notify_sap_stream(const char* stream_name,
                                              const char* multicast_ip,
                                              const char* source_ip,
                                              uint16_t port,
                                              uint32_t sample_rate) {
    if (!stream_name || !multicast_ip || !source_ip) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "SAP stream notification: name='%s', multicast=%s, source=%s, port=%d, rate=%lu",
             stream_name, multicast_ip, source_ip, port, sample_rate);
    
    // Check if this stream matches our configured stream name
    const char* configured_stream = lifecycle_get_sap_stream_name();
    if (!configured_stream || strlen(configured_stream) == 0) {
        ESP_LOGI(TAG, "No SAP stream configured for auto-join");
        return ESP_OK;
    }
    
    if (strcmp(stream_name, configured_stream) != 0) {
        ESP_LOGD(TAG, "SAP stream '%s' does not match configured stream '%s'",
                 stream_name, configured_stream);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "SAP stream '%s' matches configured stream, configuring network", stream_name);
    
    // If sample rate differs from current configuration, update it
    app_config_t *config = config_manager_get_config();
    if (sample_rate != 0 && config->sample_rate != sample_rate) {
        ESP_LOGI(TAG, "SAP stream indicates sample rate %lu Hz, updating configuration", sample_rate);
        lifecycle_manager_change_sample_rate(sample_rate);
    }
    
    // Configure the network for this stream (will determine multicast vs unicast)
    esp_err_t ret = network_configure_stream(multicast_ip, source_ip, port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure network for SAP stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Post event to notify state machine
    lifecycle_manager_post_event(LIFECYCLE_EVENT_SAP_STREAM_FOUND);
    
    return ESP_OK;
}

const char* lifecycle_get_sap_stream_name(void) {
    app_config_t *config = config_manager_get_config();
    return config->sap_stream_name;
}

esp_err_t lifecycle_set_sap_stream_name(const char* stream_name) {
    if (!stream_name) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t *config = config_manager_get_config();
    if (strcmp(config->sap_stream_name, stream_name) != 0) {
        ESP_LOGI(TAG, "Setting SAP stream name to '%s'", stream_name);

        esp_err_t ret = config_manager_save_setting("sap_stream", (void*)stream_name, strlen(stream_name) + 1);
        if (ret == ESP_OK) {
            // Trigger SAP stream configuration check
            sap_listener_check_stream_config();
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}