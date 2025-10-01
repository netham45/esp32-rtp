#include "logs_routes.h"
#include "route_helpers.h"
#include "logging/log_buffer.h"
#include <esp_log.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

static const char *TAG = "logs_routes";

/**
 * @brief Handler for GET /api/logs
 * 
 * Returns system logs from the log buffer with optional filtering and clearing.
 * 
 * Query parameters:
 * - lines: Number of lines to return (default: 50, max: 200)
 * - clear: Clear the buffer after reading (default: false)
 * - level: Filter logs by minimum level at display time (default: none)
 * - capture_level: Set log capture level (default: no change)
 * 
 * Response format:
 * {
 *   "logs": "...",
 *   "overflow": true/false,
 *   "buffer_used": 1234,
 *   "buffer_size": 8192,
 *   "capture_level": "INFO"
 * }
 */
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/logs");

    // Parse query parameters
    char query_buf[256] = {0};
    size_t buf_len = sizeof(query_buf);
    httpd_req_get_url_query_str(req, query_buf, buf_len);

    // Parse 'lines' parameter (default: 50, max: 200)
    int lines_requested = 50;
    char param_buf[32] = {0};
    if (httpd_query_key_value(query_buf, "lines", param_buf, sizeof(param_buf)) == ESP_OK) {
        int parsed_lines = atoi(param_buf);
        if (parsed_lines > 0 && parsed_lines <= 200) {
            lines_requested = parsed_lines;
        }
    }

    // Parse 'clear' parameter
    bool clear_after_read = false;
    if (httpd_query_key_value(query_buf, "clear", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strcmp(param_buf, "true") == 0 || strcmp(param_buf, "1") == 0) {
            clear_after_read = true;
        }
    }

    // Parse 'level' parameter for display filtering (ERROR, WARN, INFO, DEBUG, VERBOSE)
    esp_log_level_t display_min_level = ESP_LOG_VERBOSE;  // Default to show all
    if (httpd_query_key_value(query_buf, "level", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strcasecmp(param_buf, "ERROR") == 0) {
            display_min_level = ESP_LOG_ERROR;
        } else if (strcasecmp(param_buf, "WARN") == 0) {
            display_min_level = ESP_LOG_WARN;
        } else if (strcasecmp(param_buf, "INFO") == 0) {
            display_min_level = ESP_LOG_INFO;
        } else if (strcasecmp(param_buf, "DEBUG") == 0) {
            display_min_level = ESP_LOG_DEBUG;
        } else if (strcasecmp(param_buf, "VERBOSE") == 0) {
            display_min_level = ESP_LOG_VERBOSE;
        }
        ESP_LOGI(TAG, "Display filter level set to: %d", display_min_level);
    }

    // Parse 'capture_level' parameter to change what gets stored in buffer
    if (httpd_query_key_value(query_buf, "capture_level", param_buf, sizeof(param_buf)) == ESP_OK) {
        esp_log_level_t new_capture_level = ESP_LOG_INFO;  // Default
        if (strcasecmp(param_buf, "ERROR") == 0) {
            new_capture_level = ESP_LOG_ERROR;
        } else if (strcasecmp(param_buf, "WARN") == 0) {
            new_capture_level = ESP_LOG_WARN;
        } else if (strcasecmp(param_buf, "INFO") == 0) {
            new_capture_level = ESP_LOG_INFO;
        } else if (strcasecmp(param_buf, "DEBUG") == 0) {
            new_capture_level = ESP_LOG_DEBUG;
        } else if (strcasecmp(param_buf, "VERBOSE") == 0) {
            new_capture_level = ESP_LOG_VERBOSE;
        }
        
        // Set the new capture level
        esp_err_t err = log_buffer_set_min_level(new_capture_level);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Changed log capture level to: %d", new_capture_level);
        } else {
            ESP_LOGW(TAG, "Failed to change log capture level: 0x%x", err);
        }
    }

    // Use a fixed reasonable buffer size to avoid memory issues
    // Maximum 8KB for log data to be conservative with ESP32 memory
    size_t max_read_size = 8192;  // 8KB fixed buffer
    
    // Get current buffer status
    size_t buffer_used = log_buffer_get_used();
    size_t buffer_size = log_buffer_get_size();
    bool has_overflowed = log_buffer_has_overflowed();
    esp_log_level_t current_capture_level = log_buffer_get_min_level();

    // Limit read size to what's actually available
    if (max_read_size > buffer_used) {
        max_read_size = buffer_used;
    }

    // Allocate buffer for reading logs
    char *log_data = malloc(max_read_size + 1);
    if (!log_data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Read logs from buffer (use peek_latest to get newest logs)
    size_t bytes_read;
    if (clear_after_read) {
        // For clear, we still need to read all and clear, but we'll read the latest first
        bytes_read = log_buffer_peek_latest(log_data, max_read_size);
        log_buffer_clear();  // Clear the entire buffer after reading
        ESP_LOGI(TAG, "Read latest %zu bytes and cleared log buffer", bytes_read);
    } else {
        bytes_read = log_buffer_peek_latest(log_data, max_read_size);
        ESP_LOGD(TAG, "Peeked latest %zu bytes from log buffer", bytes_read);
    }
    log_data[bytes_read] = '\0';

    // Apply display-time level filtering if needed
    if (display_min_level < ESP_LOG_VERBOSE && bytes_read > 0) {
        // Filter logs line by line based on log level prefix
        char *filtered_data = malloc(max_read_size + 1);
        if (filtered_data) {
            size_t filtered_pos = 0;
            char *line_start = log_data;
            char *line_end;
            
            while ((line_end = strchr(line_start, '\n')) != NULL) {
                *line_end = '\0';  // Temporarily null-terminate the line
                
                // Check if this line should be displayed based on level
                bool should_display = true;
                
                // Check for ESP-IDF log level prefixes (E/W/I/D/V)
                if (line_start[0] == 'E' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_ERROR >= display_min_level);
                } else if (line_start[0] == 'W' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_WARN >= display_min_level);
                } else if (line_start[0] == 'I' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_INFO >= display_min_level);
                } else if (line_start[0] == 'D' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_DEBUG >= display_min_level);
                } else if (line_start[0] == 'V' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_VERBOSE >= display_min_level);
                }
                
                // Add line to filtered output if it passes the filter
                if (should_display) {
                    size_t line_len = strlen(line_start);
                    if (filtered_pos + line_len + 1 < max_read_size) {
                        memcpy(filtered_data + filtered_pos, line_start, line_len);
                        filtered_pos += line_len;
                        filtered_data[filtered_pos++] = '\n';
                    }
                }
                
                *line_end = '\n';  // Restore the newline
                line_start = line_end + 1;
            }
            
            // Handle last line if it doesn't end with newline
            if (*line_start != '\0') {
                bool should_display = true;
                
                if (line_start[0] == 'E' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_ERROR >= display_min_level);
                } else if (line_start[0] == 'W' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_WARN >= display_min_level);
                } else if (line_start[0] == 'I' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_INFO >= display_min_level);
                } else if (line_start[0] == 'D' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_DEBUG >= display_min_level);
                } else if (line_start[0] == 'V' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_VERBOSE >= display_min_level);
                }
                
                if (should_display) {
                    size_t line_len = strlen(line_start);
                    if (filtered_pos + line_len < max_read_size) {
                        memcpy(filtered_data + filtered_pos, line_start, line_len);
                        filtered_pos += line_len;
                    }
                }
            }
            
            filtered_data[filtered_pos] = '\0';
            
            // Replace original data with filtered data
            memcpy(log_data, filtered_data, filtered_pos + 1);
            bytes_read = filtered_pos;
            free(filtered_data);
        }
    }

    // If we need to limit to a specific number of lines, process the data
    if (bytes_read > 0 && lines_requested < 200) {
        // Count lines from the end of the buffer to get the most recent ones
        int line_count = 0;
        char *line_start = NULL;
        
        // Start from the end and work backwards to find the nth line
        for (int i = bytes_read - 1; i >= 0; i--) {
            if (log_data[i] == '\n' || i == 0) {
                line_count++;
                if (line_count == lines_requested) {
                    line_start = (i == 0) ? &log_data[0] : &log_data[i + 1];
                    break;
                }
            }
        }
        
        // If we found the starting point for the requested lines
        if (line_start && line_start != log_data) {
            size_t offset = line_start - log_data;
            memmove(log_data, line_start, bytes_read - offset + 1);
            bytes_read = bytes_read - offset;
            log_data[bytes_read] = '\0';
        }
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(log_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add log data and status to JSON
    cJSON_AddStringToObject(root, "logs", log_data);
    cJSON_AddBoolToObject(root, "overflow", has_overflowed);
    cJSON_AddNumberToObject(root, "buffer_used", buffer_used);
    cJSON_AddNumberToObject(root, "buffer_size", buffer_size);
    
    // Add current capture level to response
    const char *capture_level_str;
    switch (current_capture_level) {
        case ESP_LOG_ERROR:
            capture_level_str = "ERROR";
            break;
        case ESP_LOG_WARN:
            capture_level_str = "WARN";
            break;
        case ESP_LOG_INFO:
            capture_level_str = "INFO";
            break;
        case ESP_LOG_DEBUG:
            capture_level_str = "DEBUG";
            break;
        case ESP_LOG_VERBOSE:
            capture_level_str = "VERBOSE";
            break;
        default:
            capture_level_str = "UNKNOWN";
            break;
    }
    cJSON_AddStringToObject(root, "capture_level", capture_level_str);

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    free(log_data);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/**
 * @brief Register logs-related HTTP routes
 */
esp_err_t register_logs_routes(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "Invalid server handle");
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t logs_get_uri = {
        .uri       = "/api/logs",
        .method    = HTTP_GET,
        .handler   = logs_get_handler,
        .user_ctx  = NULL
    };

    esp_err_t ret = httpd_register_uri_handler(server, &logs_get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/logs: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Registered logs routes");
    return ESP_OK;
}