/**
 * @file routes.c
 * @brief Implementation of master route registration
 */

#include "routes.h"
#include <esp_log.h>

static const char *TAG = "routes";

esp_err_t register_all_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Register static file routes
    ret = register_static_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register static routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register WiFi management routes
    ret = register_wifi_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register settings routes
    ret = register_settings_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register settings routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register device discovery routes
    ret = register_discovery_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register discovery routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register pairing routes
    ret = register_pairing_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register pairing routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register battery status routes
    ret = register_battery_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register battery routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register OTA update routes
    ret = register_ota_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register log retrieval routes
    ret = register_logs_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register logs routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register SAP routes
    ret = register_sap_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register SAP routes: %s", esp_err_to_name(ret));
        return ret;
    }

    // IMPORTANT: Register captive portal routes LAST
    // The captive portal contains catch-all handlers (/* route) that must
    // be registered after all specific routes to avoid shadowing them
    ret = register_captive_portal_routes(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register captive portal routes: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "All routes registered successfully");
    return ESP_OK;
}