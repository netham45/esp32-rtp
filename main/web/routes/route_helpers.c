/**
 * @file route_helpers.c
 * @brief Implementation of shared utility functions for web server routes
 */

#include "route_helpers.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>
#include "visualizer_task.h"

static const char *TAG = "route_helpers";

/**
 * Chunked sending of HTML with placeholder replacement
 * This function sends HTML in chunks to avoid large memory allocations
 */
esp_err_t send_html_chunked(httpd_req_t *req, const char* html_start, size_t html_size)
{
    // Define chunk size - small enough to fit in memory easily
    #define CHUNK_SIZE 1024
    
    ESP_LOGI(TAG, "Sending HTML chunked, total size: %zu bytes", html_size);
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());

    // Stop LED visualizer while we may read from flash to avoid RMT encoder IRAM issues
//    bool viz_was_active = visualizer_is_active();
  //  if (viz_was_active) {
//        visualizer_deinit();
   // }
    
    // Set content type
    httpd_resp_set_type(req, "text/html");
    
    // Allocate a working buffer for chunks
    char *chunk_buffer = malloc(CHUNK_SIZE + 512); // Extra space for replacements
    if (!chunk_buffer) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
//        if (viz_was_active) visualizer_init();
        return httpd_resp_send_500(req);
    }
    
    // Get values for replacement once
    char device_name[] = "ESP32 RTP";
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = "Not configured";
    wifi_manager_get_current_ssid(ssid, sizeof(ssid));
    
    // Get connection status
    const char *status;
    char status_with_ip[64] = {0};
    wifi_manager_state_t state = wifi_manager_get_state();
    switch (state) {
        case WIFI_MANAGER_STATE_CONNECTED:
            {
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    snprintf(status_with_ip, sizeof(status_with_ip),
                            "Connected (IP: %s)", ip_str);
                    status = status_with_ip;
                } else {
                    status = "Connected";
                }
            }
            break;
        case WIFI_MANAGER_STATE_CONNECTING:
            status = "Connecting...";
            break;
        case WIFI_MANAGER_STATE_CONNECTION_FAILED:
            status = "Connection failed";
            break;
        case WIFI_MANAGER_STATE_AP_MODE:
            status = "Access Point Mode (192.168.4.1)";
            break;
        default:
            status = "Unknown";
            break;
    }
    
    // Process and send HTML in chunks
    size_t offset = 0;
    esp_err_t ret = ESP_OK;
    
    while (offset < html_size && ret == ESP_OK) {
        // Calculate chunk size for this iteration
        size_t chunk_size = (html_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (html_size - offset);
        
        // Copy chunk to buffer
        memcpy(chunk_buffer, html_start + offset, chunk_size);
        chunk_buffer[chunk_size] = '\0';
        
        // Simple inline replacement for placeholders in this chunk
        // Note: This is simplified - placeholders that span chunk boundaries won't be replaced
        char *pos;
        
        // Replace {{DEVICE_NAME}}
        if ((pos = strstr(chunk_buffer, "{{DEVICE_NAME}}")) != NULL) {
            size_t before_len = pos - chunk_buffer;
            size_t after_start = before_len + strlen("{{DEVICE_NAME}}");
            size_t after_len = chunk_size - after_start;
            
            // Create new buffer with replacement
            memmove(pos + strlen(device_name), pos + strlen("{{DEVICE_NAME}}"), after_len + 1);
            memcpy(pos, device_name, strlen(device_name));
            chunk_size = before_len + strlen(device_name) + after_len;
        }
        
        // Replace {{CURRENT_SSID}}
        if ((pos = strstr(chunk_buffer, "{{CURRENT_SSID}}")) != NULL) {
            size_t before_len = pos - chunk_buffer;
            size_t after_start = before_len + strlen("{{CURRENT_SSID}}");
            size_t after_len = chunk_size - after_start;
            
            memmove(pos + strlen(ssid), pos + strlen("{{CURRENT_SSID}}"), after_len + 1);
            memcpy(pos, ssid, strlen(ssid));
            chunk_size = before_len + strlen(ssid) + after_len;
        }
        
        // Replace {{CONNECTION_STATUS}}
        if ((pos = strstr(chunk_buffer, "{{CONNECTION_STATUS}}")) != NULL) {
            size_t before_len = pos - chunk_buffer;
            size_t after_start = before_len + strlen("{{CONNECTION_STATUS}}");
            size_t after_len = chunk_size - after_start;
            
            memmove(pos + strlen(status), pos + strlen("{{CONNECTION_STATUS}}"), after_len + 1);
            memcpy(pos, status, strlen(status));
            chunk_size = before_len + strlen(status) + after_len;
        }
        
        
        // Send this chunk
        ret = httpd_resp_send_chunk(req, chunk_buffer, chunk_size);
        
        offset += CHUNK_SIZE; // Move by original chunk size, not modified size
    }
    
    // Send final chunk to indicate end of response
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    
    free(chunk_buffer);

    // Restart visualizer after sending response
    //if (viz_was_active) {
//        visualizer_init();
    //}
    
    #undef CHUNK_SIZE
    return ret;
}

/**
 * URL decode a string (application/x-www-form-urlencoded)
 * Converts '+' to space and '%XX' hex sequences to actual characters
 */
esp_err_t url_decode(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *src_ptr = src;
    char *dst_ptr = dst;
    size_t dst_remaining = dst_size - 1; // Reserve space for null terminator
    
    while (*src_ptr && dst_remaining > 0) {
        if (*src_ptr == '+') {
            *dst_ptr = ' ';
            src_ptr++;
        } else if (*src_ptr == '%' && src_ptr[1] && src_ptr[2]) {
            // Convert hex %xx to character
            char hex[3] = {src_ptr[1], src_ptr[2], 0};
            *dst_ptr = (char)strtol(hex, NULL, 16);
            src_ptr += 3;
        } else {
            *dst_ptr = *src_ptr;
            src_ptr++;
        }
        dst_ptr++;
        dst_remaining--;
    }
    
    // Check if we ran out of space
    if (*src_ptr != '\0') {
        ESP_LOGW(TAG, "URL decode buffer too small");
        *dst = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    
    *dst_ptr = '\0';
    return ESP_OK;
}