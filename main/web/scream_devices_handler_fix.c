/**
 * GET handler for retrieving list of discovered Scream devices
 */
static esp_err_t scream_devices_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling request for /api/scream_devices");
    
    // Handle OPTIONS request for CORS
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // Try to get discovered devices
    scream_device_t devices[MAX_SCREAM_DEVICES];
    size_t device_count = 0;
    esp_err_t ret = mdns_discovery_get_devices(devices, MAX_SCREAM_DEVICES, &device_count);
    
    ESP_LOGI(TAG, "[DEBUG] mdns_discovery_get_devices returned: 0x%x, device_count=%d", ret, device_count);
    
    // Set content type for JSON response
    httpd_resp_set_type(req, "application/json");
    
    // Handle error case - use direct string literal
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mdns_discovery_get_devices failed with error 0x%x", ret);
        // Use string literal directly - compiler places it in DRAM
        const char *error_msg = "{\"status\":\"error\",\"message\":\"Discovery not available\",\"device_count\":0,\"devices\":[]}";
        ESP_LOGI(TAG, "[DEBUG] Sending error response, length=%d", strlen(error_msg));
        return httpd_resp_sendstr(req, error_msg);
    }
    
    // Handle empty list case
    if (device_count == 0) {
        ESP_LOGI(TAG, "[DEBUG] No devices found, sending empty list");
        const char *empty_msg = "{\"status\":\"success\",\"device_count\":0,\"devices\":[]}";
        return httpd_resp_sendstr(req, empty_msg);
    }
    
    // Build complex response for devices
    // Use heap allocation to ensure proper memory access
    const int buffer_size = 4096;
    char *response_buffer = malloc(buffer_size);
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        const char *error_msg = "{\"status\":\"error\",\"message\":\"Memory allocation failed\",\"device_count\":0,\"devices\":[]}";
        return httpd_resp_sendstr(req, error_msg);
    }
    
    // Initialize buffer to zeros to ensure null termination
    memset(response_buffer, 0, buffer_size);
    
    int offset = 0;
    int written = 0;
    
    // Start JSON - FIXED: Check snprintf return value properly
    written = snprintf(response_buffer, buffer_size,
                     "{\"status\":\"success\",\"device_count\":%d,\"devices\":[", device_count);
    if (written < 0 || written >= buffer_size) {
        ESP_LOGE(TAG, "Buffer overflow in initial JSON, needed %d bytes", written);
        free(response_buffer);
        const char *error_msg = "{\"status\":\"error\",\"message\":\"Response too large\",\"device_count\":0,\"devices\":[]}";
        return httpd_resp_sendstr(req, error_msg);
    }
    offset = written;
    
    // Add devices - but limit to prevent buffer overflow
    size_t devices_to_add = device_count;
    if (devices_to_add > 8) devices_to_add = 8; // Limit to 8 devices to fit in buffer
    
    for (size_t i = 0; i < devices_to_add; i++) {
        // Check if we have enough space left (reserve 300 bytes for closing and safety)
        if (offset >= (buffer_size - 300)) {
            ESP_LOGW(TAG, "Buffer space exhausted at device %d/%d", i, devices_to_add);
            break;
        }
        
        if (i > 0) {
            // Add comma - FIXED: Check return value
            written = snprintf(response_buffer + offset, buffer_size - offset, ",");
            if (written < 0 || written >= (buffer_size - offset)) {
                ESP_LOGE(TAG, "Buffer overflow adding comma");
                break;
            }
            offset += written;
        }
        
        // Format IP address
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&devices[i].ip_addr));
        
        // Get type string
        const char *type_str;
        switch (devices[i].type) {
            case SCREAM_DEVICE_SENDER:
                type_str = "sender";
                break;
            case SCREAM_DEVICE_RECEIVER:
                type_str = "receiver";
                break;
            case SCREAM_DEVICE_BRIDGE:
                type_str = "bridge";
                break;
            default:
                type_str = "unknown";
                break;
        }
        
        // Add device JSON - FIXED: Check return value properly
        written = snprintf(response_buffer + offset, buffer_size - offset,
                          "{\"hostname\":\"%s\",\"ip_address\":\"%s\",\"port\":%d,\"type\":\"%s\"}",
                          devices[i].hostname, ip_str, devices[i].port, type_str);
        if (written < 0 || written >= (buffer_size - offset)) {
            ESP_LOGE(TAG, "Buffer overflow adding device %d, needed %d bytes, had %d", 
                     i, written, buffer_size - offset);
            break;
        }
        offset += written;
    }
    
    // Close JSON - FIXED: Check return value and ensure we don't overflow
    written = snprintf(response_buffer + offset, buffer_size - offset, "]}");
    if (written < 0 || written >= (buffer_size - offset)) {
        ESP_LOGE(TAG, "Buffer overflow closing JSON, needed %d bytes", written);
        // Try to close it minimally
        if (offset < buffer_size - 3) {
            response_buffer[offset] = ']';
            response_buffer[offset + 1] = '}';
            response_buffer[offset + 2] = '\0';
            offset += 2;
        }
    } else {
        offset += written;
    }
    
    // Ensure null termination
    response_buffer[offset] = '\0';
    
    ESP_LOGI(TAG, "[DEBUG] Final JSON response length: %d bytes", offset);
    ESP_LOGI(TAG, "[DEBUG] Response buffer pointer: %p", response_buffer);
    ESP_LOGI(TAG, "[DEBUG] First 50 chars: %.50s", response_buffer);
    
    // Use httpd_resp_sendstr which handles null-terminated strings safely
    ret = httpd_resp_sendstr(req, response_buffer);
    free(response_buffer);
    return ret;
}