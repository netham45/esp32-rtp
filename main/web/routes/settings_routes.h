#ifndef SETTINGS_ROUTES_H
#define SETTINGS_ROUTES_H

#include "esp_http_server.h"

/**
 * Register settings-related HTTP routes
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_settings_routes(httpd_handle_t server);

#endif // SETTINGS_ROUTES_H