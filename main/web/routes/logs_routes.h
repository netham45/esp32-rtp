#ifndef LOGS_ROUTES_H
#define LOGS_ROUTES_H

#include <esp_http_server.h>

/**
 * @brief Register logs-related HTTP routes
 * 
 * @param server HTTP server handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t register_logs_routes(httpd_handle_t server);

#endif // LOGS_ROUTES_H