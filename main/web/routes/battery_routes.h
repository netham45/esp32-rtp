#ifndef BATTERY_ROUTES_H
#define BATTERY_ROUTES_H

#include "esp_http_server.h"
#include "esp_err.h"

/**
 * Register all battery management (BQ25895) routes with the HTTP server
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_battery_routes(httpd_handle_t server);

#endif // BATTERY_ROUTES_H