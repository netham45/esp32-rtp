/**
 * @file discovery_routes.c
 * @brief Implementation of mDNS device discovery HTTP routes
 */

#include "discovery_routes.h"
#include "route_helpers.h"
#include "mdns/mdns_discovery.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <string.h>

static const char *TAG = "discovery_routes";


/**
 * @brief Handler for retrieving list of discovered Scream devices
 * 
 * GET /api/scream_devices - Returns JSON array of discovered devices
 * OPTIONS /api/scream_devices - CORS preflight
 */
static esp_err_t scream_devices_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling request for /api/scream_devices");

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Set CORS headers for GET request
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    // Get the current list of discovered devices from the continuous discovery service
    discovered_device_t devices[MAX_DISCOVERED_DEVICES];
    size_t device_count = 0;
    esp_err_t err = mdns_discovery_get_devices(devices, MAX_DISCOVERED_DEVICES, &device_count);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get discovered devices: 0x%x", err);
        device_count = 0;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "device_count", device_count);
    cJSON *devices_array = cJSON_AddArrayToObject(root, "devices");

    for (size_t i = 0; i < device_count; i++) {
        cJSON *device_obj = cJSON_CreateObject();
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&devices[i].ip_addr));

        cJSON_AddStringToObject(device_obj, "hostname", devices[i].hostname);
        cJSON_AddStringToObject(device_obj, "ip_address", ip_str);
        cJSON_AddNumberToObject(device_obj, "port", devices[i].port);
        cJSON_AddItemToArray(devices_array, device_obj);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/**
 * @brief Handler for triggering immediate mDNS discovery scan
 * 
 * GET /api/discover_devices - Returns current list of discovered devices
 * OPTIONS /api/discover_devices - CORS preflight
 */
static esp_err_t discover_devices_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/discover_devices");

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // This endpoint now just triggers a scan and returns the results, same as /scream_devices.
    return scream_devices_handler(req);
}

/**
 * @brief Register all discovery-related routes
 * 
 * @param server HTTP server handle
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t register_discovery_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Register /api/discover_devices endpoint
    httpd_uri_t discover_devices_get_uri = {
        .uri = "/api/discover_devices",
        .method = HTTP_GET,
        .handler = discover_devices_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &discover_devices_get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/discover_devices: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t discover_devices_options_uri = {
        .uri = "/api/discover_devices",
        .method = HTTP_OPTIONS,
        .handler = discover_devices_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &discover_devices_options_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/discover_devices: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register /api/scream_devices endpoint
    httpd_uri_t scream_devices_get_uri = {
        .uri = "/api/scream_devices",
        .method = HTTP_GET,
        .handler = scream_devices_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &scream_devices_get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/scream_devices: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t scream_devices_options_uri = {
        .uri = "/api/scream_devices",
        .method = HTTP_OPTIONS,
        .handler = scream_devices_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &scream_devices_options_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /api/scream_devices: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Discovery routes registered successfully");
    return ESP_OK;
}