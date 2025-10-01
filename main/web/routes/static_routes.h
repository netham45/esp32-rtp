#ifndef STATIC_ROUTES_H
#define STATIC_ROUTES_H

#include "esp_http_server.h"
#include "esp_err.h"

/**
 * Register all static content routes (HTML, CSS, JS files)
 * 
 * @param server The HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_static_routes(httpd_handle_t server);

#endif // STATIC_ROUTES_H