#ifndef OTA_ROUTES_H
#define OTA_ROUTES_H

#include "esp_http_server.h"

/**
 * Register all OTA-related HTTP routes with the web server
 * 
 * Routes registered:
 * - POST /api/ota/upload - Upload firmware binary
 * - OPTIONS /api/ota/upload - CORS preflight
 * - GET /api/ota/status - Get OTA status and progress
 * - OPTIONS /api/ota/status - CORS preflight
 * - GET /api/ota/version - Get firmware version info
 * - OPTIONS /api/ota/version - CORS preflight
 * - POST /api/ota/rollback - Rollback to previous firmware
 * - OPTIONS /api/ota/rollback - CORS preflight
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_ota_routes(httpd_handle_t server);

#endif // OTA_ROUTES_H