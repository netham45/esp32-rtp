#ifndef WIFI_ROUTES_H
#define WIFI_ROUTES_H

#include "esp_http_server.h"

/**
 * Register all WiFi-related HTTP routes
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_wifi_routes(httpd_handle_t server);

#endif // WIFI_ROUTES_H