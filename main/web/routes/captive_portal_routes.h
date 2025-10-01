#ifndef CAPTIVE_PORTAL_ROUTES_H
#define CAPTIVE_PORTAL_ROUTES_H

#include "esp_http_server.h"

/**
 * Register all captive portal related routes
 * 
 * This includes:
 * - Apple Captive Network Assistant detection (/hotspot-detect.html)
 * - Catch-all redirect handler (*) - MUST be registered last
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_captive_portal_routes(httpd_handle_t server);

#endif // CAPTIVE_PORTAL_ROUTES_H