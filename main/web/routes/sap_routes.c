#include "sap_routes.h"
#include "esp_log.h"
#include "cJSON.h"
#include "receiver/sap_listener.h"
#include <time.h>
#include <string.h>

static const char *TAG = "sap_routes";

/**
 * GET handler for SAP announcements API endpoint
 */
static esp_err_t sap_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/sap");

    // Check if request is valid
    if (!req) {
        ESP_LOGE(TAG, "Invalid request pointer");
        return ESP_FAIL;
    }
    // Get all SAP announcements
    size_t count;
    const sap_announcement_t *announcements = sap_listener_get_all_announcements(&count);

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddBoolToObject(root, "is_running", sap_listener_is_running());
    
    cJSON *announcements_array = cJSON_CreateArray();
    if (!announcements_array) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON array");
        return ESP_FAIL;
    }

    time_t current_time = time(NULL);
    
    for (size_t i = 0; i < count; i++) {
        cJSON *announcement = cJSON_CreateObject();
        if (!announcement) {
            continue;
        }
        
        cJSON_AddStringToObject(announcement, "stream_name", announcements[i].stream_name);
        cJSON_AddStringToObject(announcement, "source_ip", announcements[i].source_ip);
        
        // Extract destination IP from session_info if available
        char dest_ip[16] = "";
        if (strlen(announcements[i].session_info) > 0) {
            // Parse "Connection: x.x.x.x" format
            const char* conn_prefix = "Connection: ";
            char* ip_start = strstr(announcements[i].session_info, conn_prefix);
            if (ip_start) {
                ip_start += strlen(conn_prefix);
                int octet1, octet2, octet3, octet4;
                if (sscanf(ip_start, "%d.%d.%d.%d", &octet1, &octet2, &octet3, &octet4) == 4) {
                    snprintf(dest_ip, sizeof(dest_ip), "%d.%d.%d.%d", octet1, octet2, octet3, octet4);
                }
            }
        }
        cJSON_AddStringToObject(announcement, "destination_ip", dest_ip);
        
        cJSON_AddNumberToObject(announcement, "port", announcements[i].port);
        cJSON_AddNumberToObject(announcement, "sample_rate", announcements[i].sample_rate);
        cJSON_AddNumberToObject(announcement, "bit_depth", 16);  // L16 is always 16-bit
        cJSON_AddBoolToObject(announcement, "active", announcements[i].active);
        cJSON_AddNumberToObject(announcement, "first_seen", announcements[i].first_seen);
        cJSON_AddNumberToObject(announcement, "last_seen", announcements[i].last_seen);
        cJSON_AddNumberToObject(announcement, "update_count", announcements[i].update_count);
        
        // Add relative times for easier display
        if (announcements[i].last_seen > 0) {
            cJSON_AddNumberToObject(announcement, "seconds_since_last_seen",
                                   current_time - announcements[i].last_seen);
        }
        if (announcements[i].first_seen > 0) {
            cJSON_AddNumberToObject(announcement, "seconds_since_first_seen",
                                   current_time - announcements[i].first_seen);
        }
        
        // Add session info if available
        if (strlen(announcements[i].session_info) > 0) {
            cJSON_AddStringToObject(announcement, "session_info", announcements[i].session_info);
        }
        
        cJSON_AddItemToArray(announcements_array, announcement);
    }
    
    cJSON_AddItemToObject(root, "announcements", announcements_array);
    
    // Convert to string and send
    char *json_str = cJSON_Print(root);
    
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");   
    httpd_resp_send(req, json_str, strlen(json_str));
    cJSON_Delete(root);
    
    return ESP_OK;
}

/**
 * Register SAP-related HTTP routes
 */
esp_err_t register_sap_routes(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "Invalid server handle");
        return ESP_ERR_INVALID_ARG;
    }

    // Register GET /api/sap
    httpd_uri_t sap_api = {
        .uri = "/api/sap",
        .method = HTTP_GET,
        .handler = sap_get_handler,
        .user_ctx = NULL
    };
    
    esp_err_t ret = httpd_register_uri_handler(server, &sap_api);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/sap handler: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "SAP routes registered successfully");
    return ESP_OK;
}