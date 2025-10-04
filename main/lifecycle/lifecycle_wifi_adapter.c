/**
 * @file lifecycle_wifi_adapter.c
 * @brief Implementation of the WiFi manager to lifecycle manager adapter
 */

#include "lifecycle_wifi_adapter.h"
#include "wifi_manager.h"
#include "../lifecycle_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "lifecycle_wifi_adapter";

/**
 * @brief Callback function that converts WiFi manager events to lifecycle events
 * 
 * This static callback receives events from the WiFi manager and translates
 * them into corresponding lifecycle manager events.
 * 
 * @param event The WiFi manager event
 * @param user_data User data pointer (unused)
 */
static void wifi_event_to_lifecycle_handler(wifi_manager_event_t* event, void* user_data) {
    if (!event) {
        ESP_LOGW(TAG, "Received NULL event from WiFi manager");
        return;
    }
    
    ESP_LOGD(TAG, "WiFi event received: type=%d", event->type);
    
    switch(event->type) {
        case WIFI_MANAGER_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi STA connected - notifying lifecycle manager");
            // Note: We wait for IP before considering WiFi fully connected
            break;
            
        case WIFI_MANAGER_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "WiFi got IP (IP: 0x%08x, GW: 0x%08x, NM: 0x%08x) - notifying lifecycle manager",
                     event->data.got_ip.ip,
                     event->data.got_ip.gateway,
                     event->data.got_ip.netmask);
            lifecycle_manager_post_event(LIFECYCLE_EVENT_WIFI_CONNECTED);
            break;
            
        case WIFI_MANAGER_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi STA disconnected (reason: %d) - notifying lifecycle manager",
                     event->data.sta_disconnected.reason);
            lifecycle_manager_post_event(LIFECYCLE_EVENT_WIFI_DISCONNECTED);
            break;
            
        case WIFI_MANAGER_EVENT_AP_STA_CONNECTED:
            ESP_LOGI(TAG, "Station connected to AP (MAC: %02x:%02x:%02x:%02x:%02x:%02x)",
                     event->data.ap_sta_connected.mac[0],
                     event->data.ap_sta_connected.mac[1],
                     event->data.ap_sta_connected.mac[2],
                     event->data.ap_sta_connected.mac[3],
                     event->data.ap_sta_connected.mac[4],
                     event->data.ap_sta_connected.mac[5]);
            // AP station events are informational only for lifecycle
            break;
            
        case WIFI_MANAGER_EVENT_AP_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Station disconnected from AP (MAC: %02x:%02x:%02x:%02x:%02x:%02x)",
                     event->data.ap_sta_disconnected.mac[0],
                     event->data.ap_sta_disconnected.mac[1],
                     event->data.ap_sta_disconnected.mac[2],
                     event->data.ap_sta_disconnected.mac[3],
                     event->data.ap_sta_disconnected.mac[4],
                     event->data.ap_sta_disconnected.mac[5]);
            // AP station events are informational only for lifecycle
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown WiFi event type: %d", event->type);
            break;
    }
}

esp_err_t lifecycle_wifi_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing WiFi adapter with lifecycle configuration");
    
    // Create WiFi AP configuration from lifecycle settings
    wifi_manager_ap_config_t ap_config = {
        .hide_when_sta_connected = lifecycle_get_hide_ap_when_connected(),
        .channel = WIFI_AP_CHANNEL,
        .max_connections = WIFI_AP_MAX_CONNECTIONS
    };
    
    // Get SSID from lifecycle configuration
    const char* ap_ssid = lifecycle_get_ap_ssid();
    if (ap_ssid) {
        strncpy(ap_config.ssid, ap_ssid, sizeof(ap_config.ssid) - 1);
        ap_config.ssid[sizeof(ap_config.ssid) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "No AP SSID configured, using default");
        strncpy(ap_config.ssid, "ESP32-AP", sizeof(ap_config.ssid) - 1);
    }
    
    // Get password from lifecycle configuration
    const char* ap_password = lifecycle_get_ap_password();
    if (ap_password) {
        strncpy(ap_config.password, ap_password, sizeof(ap_config.password) - 1);
        ap_config.password[sizeof(ap_config.password) - 1] = '\0';
    } else {
        // Empty password means open network
        ap_config.password[0] = '\0';
    }
    
    ESP_LOGI(TAG, "AP Configuration: SSID='%s', Password='%s', Hide=%d, Channel=%d, MaxConn=%d",
             ap_config.ssid,
             strlen(ap_config.password) > 0 ? "***" : "(open)",
             ap_config.hide_when_sta_connected,
             ap_config.channel,
             ap_config.max_connections);
    
    // Initialize WiFi manager with the configuration
    esp_err_t ret = wifi_manager_init_with_config(&ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register callback for WiFi events
    ret = wifi_manager_register_event_callback(wifi_event_to_lifecycle_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event callback: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WiFi adapter initialized successfully");
    return ESP_OK;
}

esp_err_t lifecycle_wifi_adapter_update_config(void) {
    ESP_LOGI(TAG, "Updating WiFi configuration from lifecycle settings");
    
    // Create new AP configuration from current lifecycle settings
    wifi_manager_ap_config_t ap_config = {
        .hide_when_sta_connected = lifecycle_get_hide_ap_when_connected(),
        .channel = WIFI_AP_CHANNEL,
        .max_connections = WIFI_AP_MAX_CONNECTIONS
    };
    
    // Get updated SSID
    const char* ap_ssid = lifecycle_get_ap_ssid();
    if (ap_ssid) {
        strncpy(ap_config.ssid, ap_ssid, sizeof(ap_config.ssid) - 1);
        ap_config.ssid[sizeof(ap_config.ssid) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "No AP SSID configured, using default");
        strncpy(ap_config.ssid, "ESP32-AP", sizeof(ap_config.ssid) - 1);
    }
    
    // Get updated password
    const char* ap_password = lifecycle_get_ap_password();
    if (ap_password) {
        strncpy(ap_config.password, ap_password, sizeof(ap_config.password) - 1);
        ap_config.password[sizeof(ap_config.password) - 1] = '\0';
    } else {
        ap_config.password[0] = '\0';
    }
    
    ESP_LOGI(TAG, "Updated AP Configuration: SSID='%s', Hide=%d",
             ap_config.ssid,
             ap_config.hide_when_sta_connected);
    
    // Apply the new configuration to the WiFi manager
    esp_err_t ret = wifi_manager_set_ap_config(&ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update WiFi configuration: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WiFi configuration updated successfully");
    return ESP_OK;
}