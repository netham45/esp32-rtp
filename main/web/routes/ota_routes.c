#include "ota_routes.h"
#include "ota/ota_manager.h"
#include "version.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "ota_routes";

/**
 * OTA Upload handler - receives firmware binary via HTTP POST
 * Handles chunked upload with progress tracking
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA upload request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Content-Length");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Check if OTA is already in progress
    // Note: We allow new updates if previous state was SUCCESS or ERROR
    ota_state_t current_state = ota_manager_get_state();

    if (current_state != OTA_STATE_IDLE &&
        current_state != OTA_STATE_SUCCESS &&
        current_state != OTA_STATE_ERROR) {
        // Truly in progress (CHECKING, DOWNLOADING, VERIFYING, APPLYING)
        ESP_LOGW(TAG, "OTA update already in progress (state: %d)", current_state);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"OTA update already in progress\"}");
        return ESP_FAIL;
    }

    // If previous update completed (SUCCESS/ERROR), clear the state
    if (current_state == OTA_STATE_SUCCESS || current_state == OTA_STATE_ERROR) {
        ESP_LOGI(TAG, "Clearing previous OTA state (%d) before starting new update", current_state);
        ota_manager_clear_status();
    }

    // Get content length
    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"No content provided\"}");
        return ESP_FAIL;
    }

    // Check if firmware size is within limits
    if (content_len > OTA_MAX_FIRMWARE_SIZE) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Firmware size exceeds maximum allowed size\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA update, firmware size: %zu bytes", content_len);

    // Start OTA update
    esp_err_t err = ota_manager_start(content_len, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA: 0x%x", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "{\"error\":\"Failed to start OTA: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, error_msg);
        return ESP_FAIL;
    }

    // Allocate buffer for receiving data
    const size_t buffer_size = 4096;
    uint8_t *buffer = malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        ota_manager_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    size_t received = 0;
    int remaining = content_len;
    bool ota_failed = false;

    // Receive and write firmware data in chunks
    while (remaining > 0) {
        // Receive chunk
        int recv_len = httpd_req_recv(req, (char *)buffer,
                                     remaining < buffer_size ? remaining : buffer_size);
        
        if (recv_len < 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Socket timeout during OTA upload");
                httpd_resp_send_408(req);
            } else {
                ESP_LOGE(TAG, "Failed to receive data: %d", recv_len);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            }
            ota_failed = true;
            break;
        }

        // Write to OTA partition
        err = ota_manager_write(buffer, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: 0x%x", err);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                    "{\"error\":\"Failed to write OTA data: %s\"}", esp_err_to_name(err));
            httpd_resp_sendstr(req, error_msg);
            ota_failed = true;
            break;
        }

        received += recv_len;
        remaining -= recv_len;

        // Log progress every 10%
        uint8_t progress = (received * 100) / content_len;
        static uint8_t last_progress = 0;
        if (progress >= last_progress + 10) {
            ESP_LOGI(TAG, "OTA progress: %d%% (%zu/%zu bytes)", progress, received, content_len);
            last_progress = progress;
        }
    }

    free(buffer);

    // Handle completion or failure
    if (ota_failed) {
        ota_manager_abort();
        return ESP_FAIL;
    }

    // Complete OTA update
    err = ota_manager_complete();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to complete OTA: 0x%x", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                "{\"error\":\"Failed to complete OTA: %s\",\"message\":\"%s\"}",
                esp_err_to_name(err), ota_manager_get_error_message());
        httpd_resp_sendstr(req, error_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update completed successfully");

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"OTA update completed. Device will restart.\"}");

    // Schedule restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/**
 * OTA Status handler - returns current OTA state and progress
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA status request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Get OTA status
    ota_status_t status;
    esp_err_t err = ota_manager_get_status(&status);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get OTA status");
        return ESP_FAIL;
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add status fields
    const char *state_str;
    switch (status.state) {
        case OTA_STATE_IDLE:
            state_str = "idle";
            break;
        case OTA_STATE_CHECKING:
            state_str = "checking";
            break;
        case OTA_STATE_DOWNLOADING:
            state_str = "downloading";
            break;
        case OTA_STATE_VERIFYING:
            state_str = "verifying";
            break;
        case OTA_STATE_APPLYING:
            state_str = "applying";
            break;
        case OTA_STATE_SUCCESS:
            state_str = "success";
            break;
        case OTA_STATE_ERROR:
            state_str = "error";
            break;
        case OTA_STATE_ROLLBACK:
            state_str = "rollback";
            break;
        default:
            state_str = "unknown";
            break;
    }

    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddNumberToObject(root, "error_code", status.error_code);
    cJSON_AddNumberToObject(root, "progress", status.progress_percent);
    cJSON_AddNumberToObject(root, "total_size", status.total_size);
    cJSON_AddNumberToObject(root, "received_size", status.received_size);
    
    if (status.error_code != OTA_ERR_NONE) {
        cJSON_AddStringToObject(root, "error_message", status.error_message);
    }
    
    if (status.state != OTA_STATE_IDLE) {
        cJSON_AddNumberToObject(root, "duration_ms", status.update_duration_ms);
    }
    
    cJSON_AddStringToObject(root, "current_version", status.firmware_version);
    
    if (strlen(status.new_version) > 0) {
        cJSON_AddStringToObject(root, "new_version", status.new_version);
    }

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

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
 * OTA Version handler - returns current firmware version and partition info
 */
static esp_err_t ota_version_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA version request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add version information
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION_STRING);
    cJSON_AddStringToObject(root, "version_full", FIRMWARE_VERSION_FULL);
    cJSON_AddNumberToObject(root, "version_major", FIRMWARE_VERSION_MAJOR);
    cJSON_AddNumberToObject(root, "version_minor", FIRMWARE_VERSION_MINOR);
    cJSON_AddNumberToObject(root, "version_patch", FIRMWARE_VERSION_PATCH);
    cJSON_AddNumberToObject(root, "build_number", FIRMWARE_BUILD_NUMBER);
    
    // Add build information
    cJSON_AddStringToObject(root, "app_name", FIRMWARE_APP_NAME);
    cJSON_AddStringToObject(root, "platform", FIRMWARE_PLATFORM);
    cJSON_AddStringToObject(root, "build_date", FIRMWARE_BUILD_DATE);
    cJSON_AddStringToObject(root, "build_time", FIRMWARE_BUILD_TIME);
    cJSON_AddStringToObject(root, "build_type", FIRMWARE_BUILD_TYPE);
    cJSON_AddStringToObject(root, "git_commit", FIRMWARE_GIT_COMMIT);
    cJSON_AddStringToObject(root, "git_branch", FIRMWARE_GIT_BRANCH);

    // Add partition information
    const esp_partition_t *running_partition = ota_manager_get_running_partition();
    const esp_partition_t *update_partition = ota_manager_get_update_partition();

    if (running_partition) {
        cJSON *running = cJSON_CreateObject();
        cJSON_AddStringToObject(running, "label", running_partition->label);
        cJSON_AddNumberToObject(running, "address", running_partition->address);
        cJSON_AddNumberToObject(running, "size", running_partition->size);
        cJSON_AddNumberToObject(running, "type", running_partition->type);
        cJSON_AddNumberToObject(running, "subtype", running_partition->subtype);
        cJSON_AddItemToObject(root, "running_partition", running);
    }

    if (update_partition) {
        cJSON *update = cJSON_CreateObject();
        cJSON_AddStringToObject(update, "label", update_partition->label);
        cJSON_AddNumberToObject(update, "address", update_partition->address);
        cJSON_AddNumberToObject(update, "size", update_partition->size);
        cJSON_AddNumberToObject(update, "type", update_partition->type);
        cJSON_AddNumberToObject(update, "subtype", update_partition->subtype);
        cJSON_AddItemToObject(root, "update_partition", update);
    }

    // Add OTA capabilities
    cJSON_AddBoolToObject(root, "can_rollback", ota_manager_can_rollback());
    cJSON_AddNumberToObject(root, "max_firmware_size", OTA_MAX_FIRMWARE_SIZE);

    // Get firmware info
    ota_firmware_info_t fw_info;
    if (ota_manager_get_firmware_info(&fw_info) == ESP_OK) {
        cJSON *firmware_info = cJSON_CreateObject();
        cJSON_AddStringToObject(firmware_info, "version", fw_info.version);
        cJSON_AddNumberToObject(firmware_info, "size", fw_info.size);
        cJSON_AddNumberToObject(firmware_info, "crc32", fw_info.crc32);
        cJSON_AddNumberToObject(firmware_info, "build_timestamp", fw_info.build_timestamp);
        cJSON_AddStringToObject(firmware_info, "build_hash", fw_info.build_hash);
        cJSON_AddBoolToObject(firmware_info, "secure_signed", fw_info.secure_signed);
        cJSON_AddItemToObject(root, "firmware_info", firmware_info);
    }

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

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
 * OTA Rollback handler - performs manual rollback to previous firmware
 */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA rollback request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Check if rollback is possible
    if (!ota_manager_can_rollback()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Rollback not available. No previous firmware found.\"}");
        return ESP_FAIL;
    }

    // Perform rollback
    esp_err_t err = ota_manager_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to perform rollback: 0x%x", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "{\"error\":\"Failed to rollback: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, error_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Rollback initiated successfully");

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Rollback initiated. Device will restart with previous firmware.\"}");

    // Schedule restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/**
 * Register all OTA-related routes with the HTTP server
 */
esp_err_t register_ota_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Register OTA upload endpoint (POST)
    httpd_uri_t ota_upload = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_upload);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/ota/upload: 0x%x", ret);
        return ret;
    }

    // Register OTA upload OPTIONS for CORS
    httpd_uri_t ota_upload_options = {
        .uri = "/api/ota/upload",
        .method = HTTP_OPTIONS,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_upload_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/ota/upload: 0x%x", ret);
        return ret;
    }

    // Register OTA status endpoint (GET)
    httpd_uri_t ota_status = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/ota/status: 0x%x", ret);
        return ret;
    }

    // Register OTA status OPTIONS for CORS
    httpd_uri_t ota_status_options = {
        .uri = "/api/ota/status",
        .method = HTTP_OPTIONS,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_status_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/ota/status: 0x%x", ret);
        return ret;
    }

    // Register OTA version endpoint (GET)
    httpd_uri_t ota_version = {
        .uri = "/api/ota/version",
        .method = HTTP_GET,
        .handler = ota_version_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_version);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/ota/version: 0x%x", ret);
        return ret;
    }

    // Register OTA version OPTIONS for CORS
    httpd_uri_t ota_version_options = {
        .uri = "/api/ota/version",
        .method = HTTP_OPTIONS,
        .handler = ota_version_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_version_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/ota/version: 0x%x", ret);
        return ret;
    }

    // Register OTA rollback endpoint (POST)
    httpd_uri_t ota_rollback = {
        .uri = "/api/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_rollback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/ota/rollback: 0x%x", ret);
        return ret;
    }

    // Register OTA rollback OPTIONS for CORS
    httpd_uri_t ota_rollback_options = {
        .uri = "/api/ota/rollback",
        .method = HTTP_OPTIONS,
        .handler = ota_rollback_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ota_rollback_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/ota/rollback: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "OTA routes registered successfully");
    return ESP_OK;
}