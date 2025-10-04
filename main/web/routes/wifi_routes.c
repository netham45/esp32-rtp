#include "wifi_routes.h"
#include "route_helpers.h"
#include "wifi_manager.h"
#include "lifecycle_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "cJSON.h"
#include <string.h>

#include "usb/uac_host.h"
extern uac_host_device_handle_t s_spk_dev_handle; // DAC handle from usb_audio_player_main.c

static const char *TAG = "wifi_routes";

// Maximum number of networks to return in scan
#define MAX_SCAN_RESULTS 20

/**
 * GET handler for scanning available WiFi networks
 */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /scan");

    // Scan for networks
    wifi_network_info_t networks[MAX_SCAN_RESULTS];
    size_t networks_found = 0;

    esp_err_t ret = wifi_manager_scan_networks(networks, MAX_SCAN_RESULTS, &networks_found);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to scan networks");
        return ESP_FAIL;
    }

    // Create JSON response
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Deduplicate networks in-place - O(n²) but with no extra memory usage
    // Mark duplicates by setting their SSID to empty string
    for (size_t i = 0; i < networks_found; i++) {
        // Skip if this network is already marked as a duplicate or has an empty SSID
        if (strlen(networks[i].ssid) == 0) {
            continue;
        }

        // Look for duplicates of this network
        for (size_t j = i + 1; j < networks_found; j++) {
            if (strlen(networks[j].ssid) > 0 && strcmp(networks[i].ssid, networks[j].ssid) == 0) {
                // Same SSID - keep the one with stronger signal
                if (networks[j].rssi > networks[i].rssi) {
                    // j has stronger signal, copy to i and mark j as duplicate
                    networks[i].rssi = networks[j].rssi;
                    networks[i].authmode = networks[j].authmode;
                    networks[j].ssid[0] = '\0'; // Mark as duplicate
                } else {
                    // i has stronger signal, mark j as duplicate
                    networks[j].ssid[0] = '\0'; // Mark as duplicate
                }
            }
        }
    }

    // Add networks to JSON array (only the non-duplicates)
    for (size_t i = 0; i < networks_found; i++) {
        // Skip networks marked as duplicates (empty SSID)
        if (strlen(networks[i].ssid) == 0) {
            continue;
        }

        cJSON *network = cJSON_CreateObject();
        if (!network) {
            continue;
        }

        cJSON_AddStringToObject(network, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", networks[i].rssi);
        cJSON_AddNumberToObject(network, "auth", networks[i].authmode);

        cJSON_AddItemToArray(root, network);
    }

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    ret = httpd_resp_send(req, json_str, strlen(json_str));

    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);

    return ret;
}

/**
 * POST handler for connecting to a WiFi network
 */
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /connect");

    // Get content length
    size_t content_len = req->content_len;
    if (content_len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    // Read the data
    char buf[512];
    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[content_len] = '\0';

    // Parse the form data
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};

    char *param = strstr(buf, "ssid=");
    if (param) {
        param += 5; // Skip "ssid="
        char *end = strchr(param, '&');
        if (end) {
            *end = '\0';
        }

        // URL decode using helper function
        char *decoded = malloc(strlen(param) + 1);
        if (!decoded) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        url_decode(param, decoded, strlen(param) + 1);
        strncpy(ssid, decoded, WIFI_SSID_MAX_LENGTH);
        free(decoded);

        if (end) {
            *end = '&';
        }
    }

    param = strstr(buf, "password=");
    if (param) {
        param += 9; // Skip "password="
        char *end = strchr(param, '&');
        if (end) {
            *end = '\0';
        }

        // URL decode using helper function
        char *decoded = malloc(strlen(param) + 1);
        if (!decoded) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        url_decode(param, decoded, strlen(param) + 1);
        strncpy(password, decoded, WIFI_PASSWORD_MAX_LENGTH);
        free(decoded);
    }

    // Check if SSID is provided
    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    // Check if this is the first time an SSID is being saved
    bool first_time_config = !wifi_manager_has_credentials();

    // Save the credentials to NVS and try to connect
    wifi_manager_connect(ssid, password);

    // Respond with success even if connection failed
    // The client will be redirected when the device restarts or changes mode
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");

    // If this is the first time setting up WiFi and no DAC is connected,
    // put the device into deep sleep mode after a short delay
    if (first_time_config) {
        ESP_LOGI(TAG, "First-time WiFi configuration detected");

        // Check if a DAC is connected (s_spk_dev_handle is NULL when no DAC is connected)
        if (s_spk_dev_handle == NULL) {
            ESP_LOGI(TAG, "No DAC connected after initial WiFi setup, preparing for deep sleep");
            // Give a small delay for web request to complete and logs to be sent
            vTaskDelay(pdMS_TO_TICKS(2000));
            // Go to deep sleep
        } else {
            ESP_LOGI(TAG, "DAC is connected, staying awake after WiFi setup");
        }
    }

    return ESP_OK;
}

/**
 * GET handler for connection status
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /status");

    // Get connection status
    wifi_manager_state_t state = wifi_manager_get_state();

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add status string
    const char *status;
    switch (state) {
        case WIFI_MANAGER_STATE_CONNECTED:
            status = "Connected";
            break;
        case WIFI_MANAGER_STATE_CONNECTING:
            status = "Connecting...";
            break;
        case WIFI_MANAGER_STATE_CONNECTION_FAILED:
            status = "Connection failed";
            break;
        case WIFI_MANAGER_STATE_AP_MODE:
            status = "Access Point Mode";
            break;
        default:
            status = "Unknown";
            break;
    }
    cJSON_AddStringToObject(root, "status", status);

    // If connected, add the IP address
    if (state == WIFI_MANAGER_STATE_CONNECTED) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ip_str);
        }
    }

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));

    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);

    return ret;
}

/**
 * POST handler for resetting WiFi configuration
 */
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /reset");

    // Clear the stored credentials
    esp_err_t err = wifi_manager_clear_credentials();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset WiFi configuration");
        return ESP_FAIL;
    }

    // Reset app configuration to defaults
    err = lifecycle_reset_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reset app configuration: 0x%x", err);
        // Continue anyway, WiFi reset is more important
    } else {
        ESP_LOGI(TAG, "App configuration reset to defaults");
    }

    // Start AP mode
    wifi_manager_stop();
    wifi_manager_start();

    // Respond with success
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * Register all WiFi-related HTTP routes
 */
esp_err_t register_wifi_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Register scan endpoint
    httpd_uri_t scan = {
        .uri       = "/scan",
        .method    = HTTP_GET,
        .handler   = scan_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &scan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /scan handler");
        return ret;
    }

    // Register connect endpoint
    httpd_uri_t connect = {
        .uri       = "/connect",
        .method    = HTTP_POST,
        .handler   = connect_post_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &connect);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /connect handler");
        return ret;
    }

    // Register status endpoint
    httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /status handler");
        return ret;
    }

    // Register reset endpoint
    httpd_uri_t reset = {
        .uri       = "/reset",
        .method    = HTTP_POST,
        .handler   = reset_post_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &reset);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /reset handler");
        return ret;
    }

    ESP_LOGI(TAG, "WiFi routes registered successfully");
    return ESP_OK;
}