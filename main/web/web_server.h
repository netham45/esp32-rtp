#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Start the web server for WiFi configuration
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the web server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_stop(void);

/**
 * @brief Check if the web server is running
 * 
 * @return true If the web server is running
 * @return false If the web server is not running
 */
bool web_server_is_running(void);
