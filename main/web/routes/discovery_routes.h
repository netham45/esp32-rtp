/**
 * @file discovery_routes.h
 * @brief HTTP route handlers for mDNS device discovery
 * 
 * This module provides HTTP endpoints for discovering Scream devices
 * on the network using mDNS.
 */

#ifndef DISCOVERY_ROUTES_H
#define DISCOVERY_ROUTES_H

#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register discovery-related HTTP routes
 * 
 * Registers endpoints for mDNS device discovery:
 * - GET/OPTIONS /api/discover_devices - Trigger mDNS discovery scan
 * - GET/OPTIONS /api/scream_devices - Get list of discovered Scream devices
 * 
 * @param server HTTP server handle
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t register_discovery_routes(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // DISCOVERY_ROUTES_H