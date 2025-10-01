/**
 * @file routes.h
 * @brief Master include file for all web server route modules
 * 
 * This header provides a single include point for all route handlers
 * and a convenience function to register all routes at once.
 */

#ifndef ROUTES_H
#define ROUTES_H

#include <esp_http_server.h>

// Include all route module headers
#include "route_helpers.h"
#include "static_routes.h"
#include "wifi_routes.h"
#include "settings_routes.h"
#include "discovery_routes.h"
#include "pairing_routes.h"
#include "battery_routes.h"
#include "ota_routes.h"
#include "logs_routes.h"
#include "sap_routes.h"
#include "captive_portal_routes.h"

/**
 * @brief Register all web server routes
 * 
 * This convenience function calls all individual route registration functions
 * in the correct order. Note: captive portal routes are registered last as
 * they contain catch-all handlers.
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, or the first error encountered
 */
esp_err_t register_all_routes(httpd_handle_t server);

#endif // ROUTES_H