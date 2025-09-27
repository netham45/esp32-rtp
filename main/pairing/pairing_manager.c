#include "pairing_manager.h"
#include "esp_log.h"
#include "wifi/wifi_manager.h"
#include "lifecycle_manager.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "pairing_manager";

#define MAX_SCAN_RESULTS 20
#define MAX_DEVICES_TO_PAIR 10
#define TARGET_AP_PREFIX "ESP_"

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

esp_err_t pairing_manager_start(void) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Starting Batch Pairing Mode");
    ESP_LOGI(TAG, "Will pair ALL devices with prefix '%s'", TARGET_AP_PREFIX);
    ESP_LOGI(TAG, "====================================");

    // Get the master's WiFi credentials
    char master_ssid[WIFI_SSID_MAX_LENGTH + 1];
    char master_password[WIFI_PASSWORD_MAX_LENGTH + 1];

    if (wifi_manager_get_credentials(master_ssid, sizeof(master_ssid), 
                                    master_password, sizeof(master_password)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to retrieve master credentials.");
        // Notify lifecycle manager that pairing is complete
        lifecycle_manager_post_event(LIFECYCLE_EVENT_PAIRING_COMPLETE);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Master credentials retrieved for SSID: %s", master_ssid);

    // Scan for ALL target devices at once
    wifi_network_info_t networks[MAX_SCAN_RESULTS];
    size_t networks_found = 0;
    
    ESP_LOGI(TAG, "Scanning for all devices with prefix '%s'...", TARGET_AP_PREFIX);
    if (wifi_manager_scan_networks(networks, MAX_SCAN_RESULTS, &networks_found) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to scan for networks.");
        // Exit pairing mode and return to previous state
        lifecycle_manager_post_event(LIFECYCLE_EVENT_PAIRING_COMPLETE);
        return ESP_FAIL;
    }

    // Collect all devices to pair
    typedef struct {
        char ssid[33];
        int8_t rssi;
    } target_device_t;
    
    target_device_t target_devices[MAX_DEVICES_TO_PAIR];
    int device_count = 0;
    
    ESP_LOGI(TAG, "Analyzing %d networks found...", networks_found);
    for (size_t i = 0; i < networks_found && device_count < MAX_DEVICES_TO_PAIR; i++) {
        if (strncmp(networks[i].ssid, TARGET_AP_PREFIX, strlen(TARGET_AP_PREFIX)) == 0) {
            strncpy(target_devices[device_count].ssid, networks[i].ssid, 32);
            target_devices[device_count].ssid[32] = '\0';
            target_devices[device_count].rssi = networks[i].rssi;
            ESP_LOGI(TAG, "  [%d] Found: %s (RSSI: %d)", 
                    device_count + 1, target_devices[device_count].ssid, 
                    target_devices[device_count].rssi);
            device_count++;
        }
    }

    if (device_count == 0) {
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "No devices found with prefix '%s'", TARGET_AP_PREFIX);
        ESP_LOGI(TAG, "Pairing complete (nothing to pair)");
        ESP_LOGI(TAG, "====================================");
        // Notify lifecycle manager that pairing is complete
        lifecycle_manager_post_event(LIFECYCLE_EVENT_PAIRING_COMPLETE);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Found %d device(s) to configure", device_count);
    ESP_LOGI(TAG, "Starting batch configuration...");
    ESP_LOGI(TAG, "====================================");
    
    int successful_pairs = 0;
    int failed_pairs = 0;

    // Configure each device one by one
    for (int i = 0; i < device_count; i++) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "[%d/%d] Configuring: %s", 
                i + 1, device_count, target_devices[i].ssid);
        ESP_LOGI(TAG, "------------------------------------");
        
        // Disconnect from current network
        ESP_LOGI(TAG, "  Disconnecting from current network...");
        wifi_manager_stop();
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Connect to the target AP
        ESP_LOGI(TAG, "  Connecting to target AP...");
        if (wifi_manager_connect(target_devices[i].ssid, "") != ESP_OK) {
            ESP_LOGE(TAG, "  ✗ Failed to connect to %s", target_devices[i].ssid);
            failed_pairs++;
            // Try to reconnect to original network before continuing
            wifi_manager_start();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Wait for connection to be established
        ESP_LOGI(TAG, "  Waiting for connection...");
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Prepare configuration data
        char post_data[256];
        snprintf(post_data, sizeof(post_data), "ssid=%s&password=%s", 
                master_ssid, master_password);

        // Send configuration request
        ESP_LOGI(TAG, "  Sending WiFi credentials...");
        
        esp_http_client_config_t config = {
            .url = "http://192.168.4.1/connect",
            .method = HTTP_METHOD_POST,
            .timeout_ms = 10000,
            .event_handler = _http_event_handler,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "  ✗ Failed to initialize HTTP client");
            failed_pairs++;
            // Reconnect to original network
            wifi_manager_stop();
            wifi_manager_start();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code == 200 || status_code == 204 || status_code == 302) {
                ESP_LOGI(TAG, "  ✓ SUCCESS! %s configured (HTTP %d)", 
                        target_devices[i].ssid, status_code);
                successful_pairs++;
            } else {
                ESP_LOGW(TAG, "  ✗ Configuration may have failed (HTTP %d)", status_code);
                failed_pairs++;
            }
        } else {
            ESP_LOGE(TAG, "  ✗ Failed to send configuration: %s", esp_err_to_name(err));
            failed_pairs++;
        }

        esp_http_client_cleanup(client);
        
        // Small delay before moving to next device
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Reconnect to original network
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Reconnecting to original network...");
    wifi_manager_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    wifi_manager_start();

    // Print summary
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "PAIRING COMPLETE!");
    ESP_LOGI(TAG, "------------------------------------");
    ESP_LOGI(TAG, "Total devices found: %d", device_count);
    ESP_LOGI(TAG, "Successfully paired: %d", successful_pairs);
    if (failed_pairs > 0) {
        ESP_LOGI(TAG, "Failed to pair:     %d", failed_pairs);
    }
    ESP_LOGI(TAG, "====================================");

    // Notify lifecycle manager that pairing is complete
    lifecycle_manager_post_event(LIFECYCLE_EVENT_PAIRING_COMPLETE);

    return (successful_pairs > 0) ? ESP_OK : ESP_FAIL;
}
