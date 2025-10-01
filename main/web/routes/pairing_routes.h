#ifndef PAIRING_ROUTES_H
#define PAIRING_ROUTES_H

#include "esp_http_server.h"
#include "esp_err.h"

/**
 * Register all pairing-related HTTP routes
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_pairing_routes(httpd_handle_t server);

#endif // PAIRING_ROUTES_H