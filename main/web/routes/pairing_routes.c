#include "pairing_routes.h"
#include "lifecycle_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "pairing_routes";

/**
 * Handler for starting pairing mode
 */
static esp_err_t start_pairing_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Starting pairing mode via API");
    ESP_LOGI(TAG, "[DEBUG] Request pointer: %p", req);
    
    // Check for NULL request pointer
    if (!req) {
        ESP_LOGE(TAG, "Request pointer is NULL in start_pairing_handler");
        return ESP_FAIL;
    }
    
    // Set CORS headers for OPTIONS request
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // Post event to lifecycle manager to start pairing
    esp_err_t ret = lifecycle_manager_post_event(LIFECYCLE_EVENT_START_PAIRING);
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char json_response[128];
    if (ret == ESP_OK) {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"success\",\"message\":\"Pairing mode started\"}");
        httpd_resp_send(req, json_response, strlen(json_response));
    } else {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"error\",\"message\":\"Failed to start pairing mode\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, json_response, strlen(json_response));
    }
    
    return ESP_OK;
}

/**
 * Handler for cancelling pairing mode
 */
static esp_err_t cancel_pairing_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Cancelling pairing mode via API");
    ESP_LOGI(TAG, "[DEBUG] Request pointer: %p", req);
    
    // Check for NULL request pointer
    if (!req) {
        ESP_LOGE(TAG, "Request pointer is NULL in cancel_pairing_handler");
        return ESP_FAIL;
    }
    
    // Set CORS headers for OPTIONS request
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // Post event to lifecycle manager to cancel pairing
    esp_err_t ret = lifecycle_manager_post_event(LIFECYCLE_EVENT_CANCEL_PAIRING);
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char json_response[128];
    if (ret == ESP_OK) {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"success\",\"message\":\"Pairing mode cancelled\"}");
        httpd_resp_send(req, json_response, strlen(json_response));
    } else {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"error\",\"message\":\"Failed to cancel pairing mode\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, json_response, strlen(json_response));
    }
    
    return ESP_OK;
}

/**
 * Register all pairing-related HTTP routes
 */
esp_err_t register_pairing_routes(httpd_handle_t server) {
    esp_err_t ret;
    
    // Register start pairing endpoint - POST
    httpd_uri_t start_pairing_uri = {
        .uri = "/api/start_pairing",
        .method = HTTP_POST,
        .handler = start_pairing_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &start_pairing_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/start_pairing: 0x%x", ret);
        return ret;
    }
    
    // Register start pairing endpoint - OPTIONS (for CORS)
    httpd_uri_t start_pairing_options = {
        .uri = "/api/start_pairing",
        .method = HTTP_OPTIONS,
        .handler = start_pairing_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &start_pairing_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/start_pairing: 0x%x", ret);
        return ret;
    }
    
    // Register cancel pairing endpoint - POST
    httpd_uri_t cancel_pairing_uri = {
        .uri = "/api/cancel_pairing",
        .method = HTTP_POST,
        .handler = cancel_pairing_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &cancel_pairing_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/cancel_pairing: 0x%x", ret);
        return ret;
    }
    
    // Register cancel pairing endpoint - OPTIONS (for CORS)
    httpd_uri_t cancel_pairing_options = {
        .uri = "/api/cancel_pairing",
        .method = HTTP_OPTIONS,
        .handler = cancel_pairing_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &cancel_pairing_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/cancel_pairing: 0x%x", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "Pairing routes registered successfully");
    return ESP_OK;
}