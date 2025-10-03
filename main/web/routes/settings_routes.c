#include "settings_routes.h"
#include "lifecycle_manager.h"
#include "lifecycle/config.h"
#include "wifi/wifi_manager.h"
#include "cJSON.h"
#include "config.h"
#include "esp_log.h"
#include "visualizer_task.h"
#include <string.h>

#define TAG "settings_routes"

/**
 * GET handler for device settings
 */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/settings");

    // Suspend LED visualizer to avoid any RMT/DMA ISR during flash/NVS operations
    bool viz_was_active = visualizer_is_active();
    if (viz_was_active) {
        visualizer_suspend();
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        if (viz_was_active) visualizer_resume();
        return ESP_FAIL;
    }

    // Network settings - use lifecycle getter functions
    cJSON_AddNumberToObject(root, "port", lifecycle_get_port());
    cJSON_AddStringToObject(root, "hostname", lifecycle_get_hostname());
    cJSON_AddStringToObject(root, "ap_ssid", lifecycle_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_password", lifecycle_get_ap_password());
    cJSON_AddBoolToObject(root, "hide_ap_when_connected", lifecycle_get_hide_ap_when_connected());

    // Buffer settings
    cJSON_AddNumberToObject(root, "initial_buffer_size", lifecycle_get_initial_buffer_size());
    cJSON_AddNumberToObject(root, "buffer_grow_step_size", lifecycle_get_buffer_grow_step_size());
    cJSON_AddNumberToObject(root, "max_buffer_size", lifecycle_get_max_buffer_size());
    cJSON_AddNumberToObject(root, "max_grow_size", lifecycle_get_max_grow_size());

    // Audio settings
    cJSON_AddNumberToObject(root, "sample_rate", lifecycle_get_sample_rate());
    cJSON_AddNumberToObject(root, "bit_depth", lifecycle_get_bit_depth());
    cJSON_AddNumberToObject(root, "volume", lifecycle_get_volume());
    
    // Device mode
    device_mode_t mode = lifecycle_get_device_mode();
    cJSON_AddNumberToObject(root, "device_mode", mode);
    
    // Legacy compatibility - still send the boolean fields based on mode
    cJSON_AddBoolToObject(root, "enable_usb_sender", mode == MODE_SENDER_USB);
    cJSON_AddBoolToObject(root, "enable_spdif_sender", mode == MODE_SENDER_SPDIF);

    // SPDIF settings (only relevant when IS_SPDIF is defined)
#ifdef IS_SPDIF
    cJSON_AddNumberToObject(root, "spdif_data_pin", lifecycle_get_spdif_data_pin());
#endif

    // Sender settings
    cJSON_AddStringToObject(root, "sender_destination_ip", lifecycle_get_sender_destination_ip());
    cJSON_AddNumberToObject(root, "sender_destination_port", lifecycle_get_sender_destination_port());
    
    // Sleep settings
    cJSON_AddNumberToObject(root, "silence_threshold_ms", lifecycle_get_silence_threshold_ms());
    cJSON_AddNumberToObject(root, "network_check_interval_ms", lifecycle_get_network_check_interval_ms());
    cJSON_AddNumberToObject(root, "activity_threshold_packets", lifecycle_get_activity_threshold_packets());
    cJSON_AddNumberToObject(root, "silence_amplitude_threshold", lifecycle_get_silence_amplitude_threshold());
    cJSON_AddNumberToObject(root, "network_inactivity_timeout_ms", lifecycle_get_network_inactivity_timeout_ms());

    // Use Direct Write
    cJSON_AddBoolToObject(root, "use_direct_write", lifecycle_get_use_direct_write());
    
    // SAP stream name (for automatic connection to specific SAP streams)
    cJSON_AddStringToObject(root, "sap_stream_name", lifecycle_get_sap_stream_name());
    
    // Setup wizard status
    cJSON_AddBoolToObject(root, "setup_wizard_completed", lifecycle_get_setup_wizard_completed());

    // Current WiFi SSID and password (if connected)
    char current_ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char current_password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};

    // Get both SSID and password using the existing function
    if (wifi_manager_get_credentials(current_ssid, sizeof(current_ssid),
                                     current_password, sizeof(current_password)) == ESP_OK) {
        if (strlen(current_ssid) > 0) {
            cJSON_AddStringToObject(root, "ssid", current_ssid);
            cJSON_AddStringToObject(root, "password", current_password);
        }
    }

    // Device capabilities (compile-time flags)
#ifdef IS_USB
    cJSON_AddBoolToObject(root, "has_usb_capability", true);
#else
    cJSON_AddBoolToObject(root, "has_usb_capability", false);
#endif

#ifdef IS_SPDIF
    cJSON_AddBoolToObject(root, "has_spdif_capability", true);
#else
    cJSON_AddBoolToObject(root, "has_spdif_capability", false);
#endif

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        if (viz_was_active) visualizer_resume();
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));

    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);

    // Restart visualizer after sending JSON
    if (viz_was_active) {
        visualizer_resume();
    }
    return ret;
}

/**
 * POST handler for updating device settings
 */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /api/settings");

    // Get content length
    size_t content_len = req->content_len;
    if (content_len >= 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    // Read the data
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[content_len] = '\0';

    // Parse the JSON data
    cJSON *root = cJSON_Parse(buf);
    free(buf); // We don't need the raw data anymore

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Suspend LED visualizer during configuration writes to avoid RMT/IRAM issues during NVS commits
    bool viz_was_active = visualizer_is_active();
    if (viz_was_active) {
        visualizer_suspend();
    }

    // Prepare batch update structure
    lifecycle_config_update_t updates = (lifecycle_config_update_t){0};

    // Parse and prepare updates for port
    cJSON *port = cJSON_GetObjectItem(root, "port");
    if (port && cJSON_IsNumber(port)) {
        updates.update_port = true;
        updates.port = (uint16_t)port->valueint;
    }

    // Hostname
    cJSON *hostname = cJSON_GetObjectItem(root, "hostname");
    if (hostname && cJSON_IsString(hostname)) {
        updates.update_hostname = true;
        updates.hostname = hostname->valuestring;
    }

    // WiFi AP SSID
    cJSON *ap_ssid = cJSON_GetObjectItem(root, "ap_ssid");
    if (ap_ssid && cJSON_IsString(ap_ssid)) {
        updates.update_ap_ssid = true;
        updates.ap_ssid = ap_ssid->valuestring;
    }

    // WiFi AP password
    cJSON *ap_password = cJSON_GetObjectItem(root, "ap_password");
    if (ap_password && cJSON_IsString(ap_password)) {
        updates.update_ap_password = true;
        updates.ap_password = ap_password->valuestring;
    }

    // Hide AP when connected setting
    cJSON *hide_ap_when_connected = cJSON_GetObjectItem(root, "hide_ap_when_connected");
    if (hide_ap_when_connected && cJSON_IsBool(hide_ap_when_connected)) {
        updates.update_hide_ap_when_connected = true;
        updates.hide_ap_when_connected = cJSON_IsTrue(hide_ap_when_connected);
    }

    // Buffer settings
    cJSON *initial_buffer_size = cJSON_GetObjectItem(root, "initial_buffer_size");
    if (initial_buffer_size && cJSON_IsNumber(initial_buffer_size)) {
        updates.update_initial_buffer_size = true;
        updates.initial_buffer_size = (uint8_t)initial_buffer_size->valueint;
    }

    cJSON *buffer_grow_step_size = cJSON_GetObjectItem(root, "buffer_grow_step_size");
    if (buffer_grow_step_size && cJSON_IsNumber(buffer_grow_step_size)) {
        updates.update_buffer_grow_step_size = true;
        updates.buffer_grow_step_size = (uint8_t)buffer_grow_step_size->valueint;
    }

    cJSON *max_buffer_size = cJSON_GetObjectItem(root, "max_buffer_size");
    if (max_buffer_size && cJSON_IsNumber(max_buffer_size)) {
        updates.update_max_buffer_size = true;
        updates.max_buffer_size = (uint8_t)max_buffer_size->valueint;
    }

    cJSON *max_grow_size = cJSON_GetObjectItem(root, "max_grow_size");
    if (max_grow_size && cJSON_IsNumber(max_grow_size)) {
        updates.update_max_grow_size = true;
        updates.max_grow_size = (uint8_t)max_grow_size->valueint;
    }

    // Audio settings
    // Sample rate is handled separately after batch update
    cJSON *sample_rate = cJSON_GetObjectItem(root, "sample_rate");

    cJSON *bit_depth = cJSON_GetObjectItem(root, "bit_depth");
    if (bit_depth && cJSON_IsNumber(bit_depth)) {
        // Bit depth is fixed at 16, nothing to update
    }

    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (volume && cJSON_IsNumber(volume)) {
        updates.update_volume = true;
        updates.volume = (float)volume->valuedouble;
    }

    // Sleep settings
    cJSON *silence_threshold_ms = cJSON_GetObjectItem(root, "silence_threshold_ms");
    if (silence_threshold_ms && cJSON_IsNumber(silence_threshold_ms)) {
        updates.update_silence_threshold_ms = true;
        updates.silence_threshold_ms = (uint32_t)silence_threshold_ms->valueint;
    }

    cJSON *network_check_interval_ms = cJSON_GetObjectItem(root, "network_check_interval_ms");
    if (network_check_interval_ms && cJSON_IsNumber(network_check_interval_ms)) {
        updates.update_network_check_interval_ms = true;
        updates.network_check_interval_ms = (uint32_t)network_check_interval_ms->valueint;
    }

    cJSON *activity_threshold_packets = cJSON_GetObjectItem(root, "activity_threshold_packets");
    if (activity_threshold_packets && cJSON_IsNumber(activity_threshold_packets)) {
        updates.update_activity_threshold_packets = true;
        updates.activity_threshold_packets = (uint8_t)activity_threshold_packets->valueint;
    }

    cJSON *silence_amplitude_threshold = cJSON_GetObjectItem(root, "silence_amplitude_threshold");
    if (silence_amplitude_threshold && cJSON_IsNumber(silence_amplitude_threshold)) {
        updates.update_silence_amplitude_threshold = true;
        updates.silence_amplitude_threshold = (uint16_t)silence_amplitude_threshold->valueint;
    }

    cJSON *network_inactivity_timeout_ms = cJSON_GetObjectItem(root, "network_inactivity_timeout_ms");
    if (network_inactivity_timeout_ms && cJSON_IsNumber(network_inactivity_timeout_ms)) {
        updates.update_network_inactivity_timeout_ms = true;
        updates.network_inactivity_timeout_ms = (uint32_t)network_inactivity_timeout_ms->valueint;
    }

    // Device mode setting (new preferred way)
    cJSON *device_mode = cJSON_GetObjectItem(root, "device_mode");
    if (device_mode && cJSON_IsNumber(device_mode)) {
        device_mode_t mode = (device_mode_t)device_mode->valueint;
        if (mode >= MODE_RECEIVER_USB && mode <= MODE_SENDER_SPDIF) {
            updates.update_device_mode = true;
            updates.device_mode = mode;
            ESP_LOGI(TAG, "Updating device_mode to: %d", mode);
        }
    }
    
    // Legacy USB Sender settings (for backward compatibility)
    cJSON *enable_usb_sender = cJSON_GetObjectItem(root, "enable_usb_sender");
    if (enable_usb_sender && cJSON_IsBool(enable_usb_sender)) {
        if (!device_mode) {  // Only use if device_mode wasn't provided
            updates.update_enable_usb_sender = true;
            updates.enable_usb_sender = cJSON_IsTrue(enable_usb_sender);
            ESP_LOGI(TAG, "Updating USB sender enabled to: %d", updates.enable_usb_sender);
        }
    }

    cJSON *sender_destination_ip = cJSON_GetObjectItem(root, "sender_destination_ip");
    if (sender_destination_ip && cJSON_IsString(sender_destination_ip)) {
        updates.update_sender_destination_ip = true;
        updates.sender_destination_ip = sender_destination_ip->valuestring;
        ESP_LOGI(TAG, "Updating sender destination IP to: %s", updates.sender_destination_ip);
    }

    cJSON *sender_destination_port = cJSON_GetObjectItem(root, "sender_destination_port");
    if (sender_destination_port && cJSON_IsNumber(sender_destination_port)) {
        updates.update_sender_destination_port = true;
        updates.sender_destination_port = (uint16_t)sender_destination_port->valueint;
        ESP_LOGI(TAG, "Updating sender destination port to: %d", updates.sender_destination_port);
    }

    // SPDIF settings
#ifdef IS_SPDIF
    cJSON *spdif_data_pin = cJSON_GetObjectItem(root, "spdif_data_pin");
    if (spdif_data_pin && cJSON_IsNumber(spdif_data_pin)) {
        // Limit the pin number to valid GPIO range (0-39 for ESP32)
        uint8_t pin = (uint8_t)spdif_data_pin->valueint;
        if (pin <= 39) {
            updates.update_spdif_data_pin = true;
            updates.spdif_data_pin = pin;
        }
    }
#endif

    // Legacy SPDIF sender setting (for backward compatibility)
#ifdef IS_SPDIF
    cJSON *enable_spdif_sender = cJSON_GetObjectItem(root, "enable_spdif_sender");
    if (enable_spdif_sender && cJSON_IsBool(enable_spdif_sender)) {
        if (!device_mode) {  // Only use if device_mode wasn't provided
            updates.update_enable_spdif_sender = true;
            updates.enable_spdif_sender = cJSON_IsTrue(enable_spdif_sender);
            ESP_LOGI(TAG, "Updating S/PDIF sender enabled to: %d", updates.enable_spdif_sender);
        }
    }
#endif

    // Handle direct write setting
    cJSON *use_direct_write = cJSON_GetObjectItem(root, "use_direct_write");
    ESP_LOGI(TAG, "Got use_direct_write from JSON: %p", use_direct_write);
    if (use_direct_write) {
        ESP_LOGI(TAG, "use_direct_write type: %d", use_direct_write->type);
    }
    if (use_direct_write && cJSON_IsBool(use_direct_write)) {
        bool old_value = lifecycle_get_use_direct_write();
        updates.update_use_direct_write = true;
        updates.use_direct_write = cJSON_IsTrue(use_direct_write);
        ESP_LOGI(TAG, "Direct write mode changed from %d to %d", old_value, updates.use_direct_write);
    } else {
        ESP_LOGW(TAG, "Invalid or missing use_direct_write in request");
    }
    
    // Handle SAP stream name for automatic connection
    cJSON *sap_stream_name = cJSON_GetObjectItem(root, "sap_stream_name");
    if (sap_stream_name && cJSON_IsString(sap_stream_name)) {
        updates.update_sap_stream_name = true;
        updates.sap_stream_name = sap_stream_name->valuestring;
        ESP_LOGI(TAG, "SAP stream name updated to: %s", updates.sap_stream_name);
    }
    
    // Handle setup wizard completed flag
    cJSON *setup_wizard_completed = cJSON_GetObjectItem(root, "setup_wizard_completed");
    esp_err_t err = ESP_OK;
    if (setup_wizard_completed && cJSON_IsBool(setup_wizard_completed)) {
        bool completed = cJSON_IsTrue(setup_wizard_completed);
        err = lifecycle_set_setup_wizard_completed(completed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set setup wizard completed status: %s", esp_err_to_name(err));
            cJSON_Delete(root);
            if (viz_was_active) visualizer_resume();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set setup wizard status");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Setup wizard completed status changed to %d", completed);
    }

    // Apply batch updates through lifecycle_manager
    err = lifecycle_update_config_batch(&updates);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        if (viz_was_active) visualizer_resume();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update configuration");
        return ESP_FAIL;
    }

    // Handle sample rate separately if changed
    if (sample_rate && cJSON_IsNumber(sample_rate)) {
        uint32_t new_rate = (uint32_t)sample_rate->valueint;
        uint32_t current_rate = lifecycle_get_sample_rate();
        if (new_rate != current_rate) {
            ESP_LOGI(TAG, "Sample rate change requested: %lu Hz", new_rate);
            err = lifecycle_manager_change_sample_rate(new_rate);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to change sample rate: 0x%x", err);
            }
        }
    }

    // Post configuration changed event to lifecycle manager
    // The unified handler will apply all immediate changes
    ESP_LOGI(TAG, "Configuration saved, posting event to lifecycle manager");
    lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);

    cJSON_Delete(root);

    // Resume visualizer after configuration save/update completes
    if (viz_was_active) visualizer_resume();

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Settings saved successfully\"}");

    return ESP_OK;
}

/**
 * Register settings-related HTTP routes
 */
esp_err_t register_settings_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // GET /api/settings
    httpd_uri_t settings_get = {
        .uri       = "/api/settings",
        .method    = HTTP_GET,
        .handler   = settings_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &settings_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/settings: 0x%x", ret);
        return ret;
    }

    // POST /api/settings
    httpd_uri_t settings_post = {
        .uri       = "/api/settings",
        .method    = HTTP_POST,
        .handler   = settings_post_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &settings_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/settings: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Settings routes registered successfully");
    return ESP_OK;
}