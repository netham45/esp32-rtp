#include "config.h"
#include "lifecycle_internal.h"
#include "../lifecycle_manager.h"
#include "../global.h"
#include "../config/config_manager.h"
#include "wifi_manager.h"
#include "lifecycle_wifi_adapter.h"
#include "../mdns/mdns_service.h"
#include "../mdns/mdns_discovery.h"
#include "../sender/network_out.h"
#include "../receiver/network_in.h"
#include "../receiver/audio_out.h"
#include "../receiver/buffer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <string.h>


// Forward declarations for reconfiguration functions
static esp_err_t buffer_size_reconfigure(void);

// External functions from audio_out.c for playback control
extern void stop_playback(void);
extern void resume_playback(void);

// ============================================================================
// CONFIGURATION GETTER FUNCTIONS
// ============================================================================

uint16_t lifecycle_get_port(void) {
    app_config_t *config = config_manager_get_config();
    return config->port;
}

const char* lifecycle_get_hostname(void) {
    app_config_t *config = config_manager_get_config();
    return config->hostname;
}

uint32_t lifecycle_get_sample_rate(void) {
    app_config_t *config = config_manager_get_config();
    return config->sample_rate;
}

uint8_t lifecycle_get_bit_depth(void) {
    app_config_t *config = config_manager_get_config();
    return config->bit_depth;
}

float lifecycle_get_volume(void) {
    app_config_t *config = config_manager_get_config();
    return config->volume;
}

device_mode_t lifecycle_get_device_mode(void) {
    app_config_t *config = config_manager_get_config();
    return config->device_mode;
}

bool lifecycle_get_enable_usb_sender(void) {
    app_config_t *config = config_manager_get_config();
    return config->device_mode == MODE_SENDER_USB;
}

bool lifecycle_get_enable_spdif_sender(void) {
    app_config_t *config = config_manager_get_config();
    return config->device_mode == MODE_SENDER_SPDIF;
}

const char* lifecycle_get_ap_ssid(void) {
    app_config_t *config = config_manager_get_config();
    return config->ap_ssid;
}

const char* lifecycle_get_ap_password(void) {
    app_config_t *config = config_manager_get_config();
    return config->ap_password;
}

bool lifecycle_get_hide_ap_when_connected(void) {
    app_config_t *config = config_manager_get_config();
    return config->hide_ap_when_connected;
}

const char* lifecycle_get_sender_destination_ip(void) {
    app_config_t *config = config_manager_get_config();
    return config->sender_destination_ip;
}

uint16_t lifecycle_get_sender_destination_port(void) {
    app_config_t *config = config_manager_get_config();
    return config->sender_destination_port;
}

uint8_t lifecycle_get_initial_buffer_size(void) {
    app_config_t *config = config_manager_get_config();
    return config->initial_buffer_size;
}

uint8_t lifecycle_get_max_buffer_size(void) {
    app_config_t *config = config_manager_get_config();
    return config->max_buffer_size;
}

uint8_t lifecycle_get_buffer_grow_step_size(void) {
    app_config_t *config = config_manager_get_config();
    return config->buffer_grow_step_size;
}

uint8_t lifecycle_get_max_grow_size(void) {
    app_config_t *config = config_manager_get_config();
    return config->max_grow_size;
}

uint8_t lifecycle_get_spdif_data_pin(void) {
    app_config_t *config = config_manager_get_config();
    return config->spdif_data_pin;
}

bool lifecycle_get_use_direct_write(void) {
    app_config_t *config = config_manager_get_config();
    return config->use_direct_write;
}

uint32_t lifecycle_get_silence_threshold_ms(void) {
    app_config_t *config = config_manager_get_config();
    return config->silence_threshold_ms;
}

uint32_t lifecycle_get_network_check_interval_ms(void) {
    app_config_t *config = config_manager_get_config();
    return config->network_check_interval_ms;
}

uint8_t lifecycle_get_activity_threshold_packets(void) {
    app_config_t *config = config_manager_get_config();
    return config->activity_threshold_packets;
}

uint16_t lifecycle_get_silence_amplitude_threshold(void) {
    app_config_t *config = config_manager_get_config();
    return config->silence_amplitude_threshold;
}

uint32_t lifecycle_get_network_inactivity_timeout_ms(void) {
    app_config_t *config = config_manager_get_config();
    return config->network_inactivity_timeout_ms;
}

bool lifecycle_get_enable_mdns_discovery(void) {
    app_config_t *config = config_manager_get_config();
    return config->enable_mdns_discovery;
}

uint32_t lifecycle_get_discovery_interval_ms(void) {
    app_config_t *config = config_manager_get_config();
    return config->discovery_interval_ms;
}

bool lifecycle_get_auto_select_best_device(void) {
    app_config_t *config = config_manager_get_config();
    return config->auto_select_best_device;
}

// ============================================================================
// CONFIGURATION SETTER FUNCTIONS
// ============================================================================

esp_err_t lifecycle_set_port(uint16_t port) {
    app_config_t *config = config_manager_get_config();
    if (config->port != port) {
        ESP_LOGI(TAG, "Setting port to %d", port);
        uint16_t old_port = config->port;
        config->port = port;
        esp_err_t ret = config_manager_save_setting("port", &port, sizeof(port));
        if (ret == ESP_OK) {
            // If in receiver mode and port changed, restart network
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
                state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
                ESP_LOGI(TAG, "Port changed from %d to %d in receiver mode, restarting network", old_port, port);
                network_update_port();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_hostname(const char* hostname) {
    if (!hostname) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t *config = config_manager_get_config();
    if (strcmp(config->hostname, hostname) != 0) {
        ESP_LOGI(TAG, "Setting hostname to %s", hostname);
        strncpy(config->hostname, hostname, sizeof(config->hostname) - 1);
        config->hostname[sizeof(config->hostname) - 1] = '\0';
        esp_err_t ret = config_manager_save_setting("hostname", config->hostname, sizeof(config->hostname));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_volume(float volume) {
    if (volume < 0.0f || volume > 1.0f) {
        ESP_LOGE(TAG, "Invalid volume value: %f", volume);
        return ESP_ERR_INVALID_ARG;
    }
    
    app_config_t *config = config_manager_get_config();
    if (config->volume != volume) {
        ESP_LOGI(TAG, "Setting volume to %.2f", volume);
        config->volume = volume;
        esp_err_t ret = config_manager_save_setting("volume", &volume, sizeof(volume));
        if (ret == ESP_OK) {
            // Apply volume change immediately if in receiver USB mode
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB) {
                audio_out_update_volume();
            }
            // Post event for notification
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_device_mode(device_mode_t mode) {
    app_config_t *config = config_manager_get_config();
    if (config->device_mode != mode) {
        ESP_LOGI(TAG, "Setting device mode to %d", mode);
        config->device_mode = mode;
        esp_err_t ret = config_manager_save_setting("device_mode", &mode, sizeof(mode));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_enable_usb_sender(bool enable) {
    app_config_t *config = config_manager_get_config();
    // When disabling USB sender, default to receiver mode based on build type
    device_mode_t new_mode;
    if (enable) {
        new_mode = MODE_SENDER_USB;
    } else {
        new_mode = MODE_RECEIVER_USB;
    }
    if (config->device_mode != new_mode) {
        ESP_LOGI(TAG, "Setting USB sender enabled to %d (device_mode=%d)", enable, new_mode);
        config->device_mode = new_mode;
        esp_err_t ret = config_manager_save_setting("device_mode", &new_mode, sizeof(new_mode));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_enable_spdif_sender(bool enable) {
    app_config_t *config = config_manager_get_config();
    // When disabling SPDIF sender, default to receiver mode based on build type
    device_mode_t new_mode;
    if (enable) {
        new_mode = MODE_SENDER_SPDIF;
    } else {
        new_mode = MODE_RECEIVER_USB;
    }
    if (config->device_mode != new_mode) {
        ESP_LOGI(TAG, "Setting SPDIF sender enabled to %d (device_mode=%d)", enable, new_mode);
        config->device_mode = new_mode;
        esp_err_t ret = config_manager_save_setting("device_mode", &new_mode, sizeof(new_mode));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_ap_ssid(const char* ssid) {
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    
    app_config_t *config = config_manager_get_config();
    if (strcmp(config->ap_ssid, ssid) != 0) {
        ESP_LOGI(TAG, "Setting AP SSID to %s", ssid);
        strncpy(config->ap_ssid, ssid, WIFI_SSID_MAX_LENGTH);
        config->ap_ssid[WIFI_SSID_MAX_LENGTH] = '\0';
        esp_err_t ret = config_manager_save_setting("ap_ssid", config->ap_ssid, sizeof(config->ap_ssid));
        if (ret == ESP_OK) {
            // Update WiFi adapter configuration with new AP settings
            lifecycle_wifi_adapter_update_config();
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_ap_password(const char* password) {
    if (!password) {
        return ESP_ERR_INVALID_ARG;
    }
    
    app_config_t *config = config_manager_get_config();
    if (strcmp(config->ap_password, password) != 0) {
        ESP_LOGI(TAG, "Setting AP password");
        strncpy(config->ap_password, password, WIFI_PASSWORD_MAX_LENGTH);
        config->ap_password[WIFI_PASSWORD_MAX_LENGTH] = '\0';
        esp_err_t ret = config_manager_save_setting("ap_password", config->ap_password, sizeof(config->ap_password));
        if (ret == ESP_OK) {
            // Update WiFi adapter configuration with new AP settings
            lifecycle_wifi_adapter_update_config();
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_hide_ap_when_connected(bool hide) {
    app_config_t *config = config_manager_get_config();
    if (config->hide_ap_when_connected != hide) {
        ESP_LOGI(TAG, "Setting hide_ap_when_connected to %d", hide);
        config->hide_ap_when_connected = hide;
        esp_err_t ret = config_manager_save_setting("hide_ap_when_connected", &hide, sizeof(hide));
        if (ret == ESP_OK) {
            // Update WiFi adapter configuration with new AP settings
            lifecycle_wifi_adapter_update_config();
            
            // Apply AP visibility change immediately if connected
            wifi_manager_state_t wifi_state = wifi_manager_get_state();
            if (wifi_state == WIFI_MANAGER_STATE_CONNECTED) {
                if (hide) {
                    ESP_LOGI(TAG, "Hiding AP interface (WiFi is connected)");
                    esp_wifi_set_mode(WIFI_MODE_STA);
                } else {
                    ESP_LOGI(TAG, "Showing AP interface");
                    esp_wifi_set_mode(WIFI_MODE_APSTA);
                }
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_sender_destination_ip(const char* ip) {
    if (!ip) {
        return ESP_ERR_INVALID_ARG;
    }
    
    app_config_t *config = config_manager_get_config();
    if (strcmp(config->sender_destination_ip, ip) != 0) {
        ESP_LOGI(TAG, "Setting sender destination IP to %s", ip);
        strncpy(config->sender_destination_ip, ip, 15);
        config->sender_destination_ip[15] = '\0';
        esp_err_t ret = config_manager_save_setting("sender_destination_ip", config->sender_destination_ip,
                                                    sizeof(config->sender_destination_ip));
        if (ret == ESP_OK) {
            // If we're in sender mode, update the destination immediately without restart
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_SENDER_USB ||
                state == LIFECYCLE_STATE_MODE_SENDER_SPDIF) {
                ESP_LOGI(TAG, "Updating RTP sender destination IP immediately");
                rtp_sender_update_destination();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_sender_destination_port(uint16_t port) {
    app_config_t *config = config_manager_get_config();
    if (config->sender_destination_port != port) {
        ESP_LOGI(TAG, "Setting sender destination port to %d", port);
        config->sender_destination_port = port;
        esp_err_t ret = config_manager_save_setting("sender_destination_port", &port, sizeof(port));
        if (ret == ESP_OK) {
            // If we're in sender mode, update the destination immediately without restart
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_SENDER_USB ||
                state == LIFECYCLE_STATE_MODE_SENDER_SPDIF) {
                ESP_LOGI(TAG, "Updating RTP sender destination port immediately");
                rtp_sender_update_destination();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_initial_buffer_size(uint8_t size) {
    app_config_t *config = config_manager_get_config();
    if (config->initial_buffer_size != size) {
        ESP_LOGI(TAG, "Setting initial_buffer_size to %d", size);
        config->initial_buffer_size = size;
        esp_err_t ret = config_manager_save_setting("initial_buffer_size", &size, sizeof(size));
        if (ret == ESP_OK) {
            // If in receiver mode, reconfigure buffer immediately
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
                state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
                ESP_LOGI(TAG, "Updating buffer configuration immediately");
                buffer_size_reconfigure();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_max_buffer_size(uint8_t size) {
    app_config_t *config = config_manager_get_config();
    if (config->max_buffer_size != size) {
        ESP_LOGI(TAG, "Setting max_buffer_size to %d", size);
        config->max_buffer_size = size;
        esp_err_t ret = config_manager_save_setting("max_buffer_size", &size, sizeof(size));
        if (ret == ESP_OK) {
            // If in receiver mode, reconfigure buffer immediately
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
                state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
                ESP_LOGI(TAG, "Updating buffer configuration immediately");
                buffer_size_reconfigure();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_buffer_grow_step_size(uint8_t size) {
    app_config_t *config = config_manager_get_config();
    if (config->buffer_grow_step_size != size) {
        ESP_LOGI(TAG, "Setting buffer_grow_step_size to %d", size);
        config->buffer_grow_step_size = size;
        esp_err_t ret = config_manager_save_setting("buffer_grow_step_size", &size, sizeof(size));
        if (ret == ESP_OK) {
            // If in receiver mode, reconfigure buffer immediately
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
                state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
                ESP_LOGI(TAG, "Updating buffer configuration immediately");
                buffer_size_reconfigure();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_max_grow_size(uint8_t size) {
    app_config_t *config = config_manager_get_config();
    if (config->max_grow_size != size) {
        ESP_LOGI(TAG, "Setting max_grow_size to %d", size);
        config->max_grow_size = size;
        esp_err_t ret = config_manager_save_setting("max_grow_size", &size, sizeof(size));
        if (ret == ESP_OK) {
            // If in receiver mode, reconfigure buffer immediately
            lifecycle_state_t state = lifecycle_get_current_state();
            if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
                state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
                ESP_LOGI(TAG, "Updating buffer configuration immediately");
                buffer_size_reconfigure();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_spdif_data_pin(uint8_t pin) {
    if (pin > 39) { // ESP32 GPIO range
        ESP_LOGE(TAG, "Invalid SPDIF data pin: %d", pin);
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t *config = config_manager_get_config();
    if (config->spdif_data_pin != pin) {
        ESP_LOGI(TAG, "Setting SPDIF data pin to %d", pin);
        config->spdif_data_pin = pin;
        esp_err_t ret = config_manager_save_setting("spdif_data_pin", &pin, sizeof(pin));
        if (ret == ESP_OK) {
            // Notify lifecycle manager; actual application occurs elsewhere
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_silence_threshold_ms(uint32_t threshold_ms) {
    app_config_t *config = config_manager_get_config();
    if (config->silence_threshold_ms != threshold_ms) {
        ESP_LOGI(TAG, "Setting silence_threshold_ms to %lu", threshold_ms);
        config->silence_threshold_ms = threshold_ms;
        esp_err_t ret = config_manager_save_setting("silence_threshold_ms", &threshold_ms, sizeof(threshold_ms));
        if (ret == ESP_OK) {
            // Sleep module will update cached value immediately if monitoring is active
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_network_check_interval_ms(uint32_t interval_ms) {
    app_config_t *config = config_manager_get_config();
    if (config->network_check_interval_ms != interval_ms) {
        ESP_LOGI(TAG, "Setting network_check_interval_ms to %lu", interval_ms);
        config->network_check_interval_ms = interval_ms;
        esp_err_t ret = config_manager_save_setting("network_check_interval_ms", &interval_ms, sizeof(interval_ms));
        if (ret == ESP_OK) {
            // Sleep module will update cached value immediately if monitoring is active
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_activity_threshold_packets(uint8_t packets) {
    app_config_t *config = config_manager_get_config();
    if (config->activity_threshold_packets != packets) {
        ESP_LOGI(TAG, "Setting activity_threshold_packets to %d", packets);
        config->activity_threshold_packets = packets;
        esp_err_t ret = config_manager_save_setting("activity_threshold_packets", &packets, sizeof(packets));
        if (ret == ESP_OK) {
            // Sleep module will update cached value immediately if monitoring is active
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_silence_amplitude_threshold(uint16_t threshold) {
    app_config_t *config = config_manager_get_config();
    if (config->silence_amplitude_threshold != threshold) {
        ESP_LOGI(TAG, "Setting silence_amplitude_threshold to %d", threshold);
        config->silence_amplitude_threshold = threshold;
        esp_err_t ret = config_manager_save_setting("silence_amplitude_threshold", &threshold, sizeof(threshold));
        if (ret == ESP_OK) {
            // Sleep module will update cached value immediately if monitoring is active
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_network_inactivity_timeout_ms(uint32_t timeout_ms) {
    app_config_t *config = config_manager_get_config();
    if (config->network_inactivity_timeout_ms != timeout_ms) {
        ESP_LOGI(TAG, "Setting network_inactivity_timeout_ms to %lu", timeout_ms);
        config->network_inactivity_timeout_ms = timeout_ms;
        esp_err_t ret = config_manager_save_setting("network_inactivity_timeout_ms", &timeout_ms, sizeof(timeout_ms));
        if (ret == ESP_OK) {
            // Sleep module will update cached value immediately if monitoring is active
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_enable_mdns_discovery(bool enable) {
    app_config_t *config = config_manager_get_config();
    if (config->enable_mdns_discovery != enable) {
        ESP_LOGI(TAG, "Setting enable_mdns_discovery to %d", enable);
        config->enable_mdns_discovery = enable;
        esp_err_t ret = config_manager_save_setting("enable_mdns_discovery", &enable, sizeof(enable));
        if (ret == ESP_OK) {
            // Start or stop mDNS discovery based on new setting
            if (enable) {
                ESP_LOGI(TAG, "Enabling mDNS discovery");
                mdns_discovery_start();
            } else {
                ESP_LOGI(TAG, "Disabling mDNS discovery");
                mdns_discovery_stop();
            }
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_discovery_interval_ms(uint32_t interval_ms) {
    app_config_t *config = config_manager_get_config();
    if (config->discovery_interval_ms != interval_ms) {
        ESP_LOGI(TAG, "Setting discovery_interval_ms to %lu", interval_ms);
        config->discovery_interval_ms = interval_ms;
        esp_err_t ret = config_manager_save_setting("discovery_interval_ms", &interval_ms, sizeof(interval_ms));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lifecycle_set_auto_select_best_device(bool enable) {
    app_config_t *config = config_manager_get_config();
    if (config->auto_select_best_device != enable) {
        ESP_LOGI(TAG, "Setting auto_select_best_device to %d", enable);
        config->auto_select_best_device = enable;
        esp_err_t ret = config_manager_save_setting("auto_select_best_device", &enable, sizeof(enable));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

// ============================================================================
// BATCH UPDATE FUNCTION
// Batch update implementation
// Applies provided fields atomically to the in-memory config and persists to NVS.
// Keeps legacy flags in sync with device_mode and triggers runtime updates where applicable.
// ============================================================================

esp_err_t lifecycle_update_config_batch(const lifecycle_config_update_t* updates) {
    if (!updates) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t *config = config_manager_get_config();

    // Resolve device mode (prefer explicit device_mode over legacy booleans)
    bool device_mode_explicit = updates->update_device_mode;
    device_mode_t resolved_mode = config->device_mode;
    if (device_mode_explicit) {
        resolved_mode = updates->device_mode;
    } else {
        // Handle legacy flags only if device_mode wasn't provided
        if (updates->update_enable_usb_sender) {
            resolved_mode = updates->enable_usb_sender
                ? MODE_SENDER_USB
                : MODE_RECEIVER_USB
            ;
        } else if (updates->update_enable_spdif_sender) {
            resolved_mode = updates->enable_spdif_sender
                ? MODE_SENDER_SPDIF
                : MODE_RECEIVER_USB
            ;
        }
    }

    // Network/basic settings
    if (updates->update_port) {
        config->port = updates->port;
    }
    if (updates->update_hostname && updates->hostname) {
        strncpy(config->hostname, updates->hostname, sizeof(config->hostname) - 1);
        config->hostname[sizeof(config->hostname) - 1] = '\0';
    }

    // AP settings
    if (updates->update_ap_ssid && updates->ap_ssid) {
        strncpy(config->ap_ssid, updates->ap_ssid, sizeof(config->ap_ssid) - 1);
        config->ap_ssid[sizeof(config->ap_ssid) - 1] = '\0';
    }
    if (updates->update_ap_password && updates->ap_password) {
        strncpy(config->ap_password, updates->ap_password, sizeof(config->ap_password) - 1);
        config->ap_password[sizeof(config->ap_password) - 1] = '\0';
    }
    if (updates->update_hide_ap_when_connected) {
        config->hide_ap_when_connected = updates->hide_ap_when_connected;
    }

    // Buffer params
    if (updates->update_initial_buffer_size) {
        config->initial_buffer_size = updates->initial_buffer_size;
    }
    if (updates->update_buffer_grow_step_size) {
        config->buffer_grow_step_size = updates->buffer_grow_step_size;
    }
    if (updates->update_max_buffer_size) {
        config->max_buffer_size = updates->max_buffer_size;
    }
    if (updates->update_max_grow_size) {
        config->max_grow_size = updates->max_grow_size;
    }

    // Audio settings (volume handled here; sample_rate handled separately by caller)
    if (updates->update_volume) {
        config->volume = updates->volume;
    }

    if (updates->update_spdif_data_pin) {
        config->spdif_data_pin = updates->spdif_data_pin;
    }

    // Sender settings
    if (updates->update_sender_destination_ip && updates->sender_destination_ip) {
        strncpy(config->sender_destination_ip, updates->sender_destination_ip, sizeof(config->sender_destination_ip) - 1);
        config->sender_destination_ip[sizeof(config->sender_destination_ip) - 1] = '\0';
    }
    if (updates->update_sender_destination_port) {
        config->sender_destination_port = updates->sender_destination_port;
    }

    // Sleep settings
    if (updates->update_silence_threshold_ms) {
        config->silence_threshold_ms = updates->silence_threshold_ms;
    }
    if (updates->update_network_check_interval_ms) {
        config->network_check_interval_ms = updates->network_check_interval_ms;
    }
    if (updates->update_activity_threshold_packets) {
        config->activity_threshold_packets = updates->activity_threshold_packets;
    }
    if (updates->update_silence_amplitude_threshold) {
        config->silence_amplitude_threshold = updates->silence_amplitude_threshold;
    }
    if (updates->update_network_inactivity_timeout_ms) {
        config->network_inactivity_timeout_ms = updates->network_inactivity_timeout_ms;
    }

    // Audio processing
    if (updates->update_use_direct_write) {
        config->use_direct_write = updates->use_direct_write;
    }

    // mDNS discovery
    if (updates->update_enable_mdns_discovery) {
        config->enable_mdns_discovery = updates->enable_mdns_discovery;
    }
    if (updates->update_discovery_interval_ms) {
        config->discovery_interval_ms = updates->discovery_interval_ms;
    }
    if (updates->update_auto_select_best_device) {
        config->auto_select_best_device = updates->auto_select_best_device;
    }

    // Setup wizard status (normally handled earlier by caller, but support here too)
    if (updates->update_setup_wizard_completed) {
        config->setup_wizard_completed = updates->setup_wizard_completed;
    }

    // SAP stream name
    if (updates->update_sap_stream_name && updates->sap_stream_name) {
        strncpy(config->sap_stream_name, updates->sap_stream_name, sizeof(config->sap_stream_name) - 1);
        config->sap_stream_name[sizeof(config->sap_stream_name) - 1] = '\0';
    }

    // Apply resolved device mode and keep legacy fields in sync
    config->device_mode = resolved_mode;
    switch (resolved_mode) {
        case MODE_RECEIVER_USB:
        case MODE_RECEIVER_SPDIF:
            config->enable_usb_sender = false;
            config->enable_spdif_sender = false;
            break;
        case MODE_SENDER_USB:
            config->enable_usb_sender = true;
            config->enable_spdif_sender = false;
            break;
        case MODE_SENDER_SPDIF:
            config->enable_usb_sender = false;
            config->enable_spdif_sender = true;
            break;
    }

    // Persist all changes in one commit
    esp_err_t ret = config_manager_save_config();
    return ret;
}

// ============================================================================
// SETUP WIZARD FUNCTIONS
// ============================================================================

bool lifecycle_get_setup_wizard_completed(void) {
    app_config_t *config = config_manager_get_config();
    return config->setup_wizard_completed;
}

esp_err_t lifecycle_set_setup_wizard_completed(bool completed) {
    app_config_t *config = config_manager_get_config();
    if (config->setup_wizard_completed != completed) {
        ESP_LOGI(TAG, "Setting setup_wizard_completed to %d", completed);
        config->setup_wizard_completed = completed;
        esp_err_t ret = config_manager_save_setting("wizard_done", &completed, sizeof(completed));
        if (ret == ESP_OK) {
            lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
        }
        return ret;
    }
    return ESP_OK;
}

// ============================================================================
// CONFIGURATION MANAGEMENT FUNCTIONS
// ============================================================================

esp_err_t lifecycle_reset_config(void) {
    ESP_LOGI(TAG, "Resetting configuration to factory defaults");
    esp_err_t ret = config_manager_reset();
    if (ret == ESP_OK) {
        lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);
    }
    return ret;
}

esp_err_t lifecycle_save_config(void) {
    ESP_LOGI(TAG, "Saving configuration to persistent storage");
    return config_manager_save_config();
}

// ============================================================================
// RECONFIGURATION HELPER FUNCTIONS (STATIC/INTERNAL)
// ============================================================================


/**
 * Reconfigure buffer sizes for active receiver modes without restart
 */
static esp_err_t buffer_size_reconfigure(void) {
    ESP_LOGI(TAG, "Reconfiguring buffer sizes without restart");
    
    lifecycle_state_t state = lifecycle_get_current_state();
    
    if (state != LIFECYCLE_STATE_MODE_RECEIVER_USB &&
        state != LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
        return ESP_ERR_INVALID_STATE;
    }
    
    stop_playback();
    vTaskDelay(pdMS_TO_TICKS(100));
    empty_buffer();
    
    esp_err_t ret = buffer_update_growth_params();
    
    resume_playback();
    return ret;
}

// ============================================================================
// UNIFIED CONFIGURATION CHANGE HANDLER
// Internal unified configuration change handler
// Detects changes vs previous snapshot, applies immediate reconfig where possible
// (network port, mDNS hostname, sender destination, buffers, SPDIF pin, volume, sleep params)
// and reports whether a restart (state re-evaluation) is required (e.g., device_mode change).
// ============================================================================

bool lifecycle_config_handle_configuration_changed(void) {
    ESP_LOGI(TAG, "Checking for configuration changes that can be applied immediately");

    // Track previous configuration across calls
    static app_config_t previous_config = {0};
    static bool first_call = true;

    app_config_t *current_config = config_manager_get_config();
    if (first_call) {
        memcpy(&previous_config, current_config, sizeof(app_config_t));
        first_call = false;
        return false; // Nothing to do on first invocation
    }

    bool restart_required = false;
    bool any_changes = false;

    lifecycle_state_t state = lifecycle_get_current_state();

    // Port changes
    if (current_config->port != previous_config.port) {
        ESP_LOGI(TAG, "Port changed from %d to %d", previous_config.port, current_config->port);
        any_changes = true;
        if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
            state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
            ESP_LOGI(TAG, "Applying port change immediately");
            network_update_port();
        }
    }

    // Hostname changes
    if (strcmp(current_config->hostname, previous_config.hostname) != 0) {
        ESP_LOGI(TAG, "Hostname changed from %s to %s", previous_config.hostname, current_config->hostname);
        any_changes = true;
        ESP_LOGI(TAG, "Updating mDNS hostname immediately");
        mdns_service_update_hostname();
    }

    // Sender destination changes
    if (strcmp(current_config->sender_destination_ip, previous_config.sender_destination_ip) != 0 ||
        current_config->sender_destination_port != previous_config.sender_destination_port) {
        ESP_LOGI(TAG, "Sender destination changed: %s:%d -> %s:%d",
                 previous_config.sender_destination_ip, previous_config.sender_destination_port,
                 current_config->sender_destination_ip, current_config->sender_destination_port);
        any_changes = true;

        if (state == LIFECYCLE_STATE_MODE_SENDER_USB ||
            state == LIFECYCLE_STATE_MODE_SENDER_SPDIF) {
            ESP_LOGI(TAG, "Updating sender destination immediately");
            rtp_sender_update_destination();
        }
    }

    // Buffer parameter changes
    if (current_config->initial_buffer_size != previous_config.initial_buffer_size ||
        current_config->max_buffer_size != previous_config.max_buffer_size ||
        current_config->buffer_grow_step_size != previous_config.buffer_grow_step_size ||
        current_config->max_grow_size != previous_config.max_grow_size) {
        ESP_LOGI(TAG, "Buffer parameters changed");
        any_changes = true;

        if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB ||
            state == LIFECYCLE_STATE_MODE_RECEIVER_SPDIF) {
            ESP_LOGI(TAG, "Reconfiguring buffer parameters immediately");
            esp_err_t ret = buffer_size_reconfigure();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Buffer reconfiguration failed, restart may be needed");
                restart_required = true;
            }
        }
    }

    // SPDIF pin changes
    if (current_config->spdif_data_pin != previous_config.spdif_data_pin) {
        ESP_LOGI(TAG, "SPDIF pin changed from %d to %d", previous_config.spdif_data_pin, current_config->spdif_data_pin);
        any_changes = true;
        // No immediate reconfiguration in config.c; require restart to apply
        restart_required = true;
    }

    // Volume changes
    if (current_config->volume != previous_config.volume) {
        ESP_LOGI(TAG, "Volume changed from %.2f to %.2f",
                 previous_config.volume, current_config->volume);
        any_changes = true;
        if (state == LIFECYCLE_STATE_MODE_RECEIVER_USB) {
            ESP_LOGI(TAG, "Applying volume change immediately");
            audio_out_update_volume();
        }
    }

    // Direct write changes
    if (current_config->use_direct_write != previous_config.use_direct_write) {
        ESP_LOGI(TAG, "Direct write mode changed from %d to %d",
                 previous_config.use_direct_write, current_config->use_direct_write);
        any_changes = true;
        // No immediate action required here
    }

    // Sleep monitoring parameter changes
    if (current_config->silence_threshold_ms != previous_config.silence_threshold_ms ||
        current_config->network_check_interval_ms != previous_config.network_check_interval_ms ||
        current_config->activity_threshold_packets != previous_config.activity_threshold_packets ||
        current_config->silence_amplitude_threshold != previous_config.silence_amplitude_threshold ||
        current_config->network_inactivity_timeout_ms != previous_config.network_inactivity_timeout_ms) {
        ESP_LOGI(TAG, "Sleep monitoring parameters changed");
        any_changes = true;

        lifecycle_context_t *ctx = lifecycle_get_context();
        if (ctx && ctx->monitoring_active) {
            ESP_LOGI(TAG, "Updating active sleep monitoring parameters");
            ctx->cached_silence_threshold_ms = current_config->silence_threshold_ms;
            ctx->cached_network_check_interval_ms = current_config->network_check_interval_ms;
            ctx->cached_activity_threshold_packets = current_config->activity_threshold_packets;
            ctx->cached_silence_amplitude_threshold = current_config->silence_amplitude_threshold;
            ctx->cached_network_inactivity_timeout_ms = current_config->network_inactivity_timeout_ms;
        }
    }

    // Device mode changes always require restart
    if (current_config->device_mode != previous_config.device_mode) {
        ESP_LOGI(TAG, "Device mode changed from %d to %d - restart required",
                 previous_config.device_mode, current_config->device_mode);
        any_changes = true;
        restart_required = true;
    }

    // AP visibility setting changes
    if (current_config->hide_ap_when_connected != previous_config.hide_ap_when_connected) {
        ESP_LOGI(TAG, "AP visibility setting changed from %d to %d",
                 previous_config.hide_ap_when_connected, current_config->hide_ap_when_connected);
        any_changes = true;

        wifi_manager_state_t wifi_state = wifi_manager_get_state();
        if (wifi_state == WIFI_MANAGER_STATE_CONNECTED) {
            if (current_config->hide_ap_when_connected) {
                ESP_LOGI(TAG, "Hiding AP interface immediately");
                esp_wifi_set_mode(WIFI_MODE_STA);
            } else {
                ESP_LOGI(TAG, "Showing AP interface immediately");
                esp_wifi_set_mode(WIFI_MODE_APSTA);
            }
        }
    }

    // Update previous config snapshot
    if (any_changes) {
        ESP_LOGI(TAG, "Configuration changes detected, updating previous config cache");
        memcpy(&previous_config, current_config, sizeof(app_config_t));
    } else {
        ESP_LOGD(TAG, "No configuration changes detected");
    }

    return restart_required;
}