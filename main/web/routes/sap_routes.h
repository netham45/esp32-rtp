#ifndef SAP_ROUTES_H
#define SAP_ROUTES_H

#include "esp_http_server.h"

/**
 * Register SAP-related HTTP routes
 * 
 * @param server HTTP server handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t register_sap_routes(httpd_handle_t server);

#endif // SAP_ROUTES_H