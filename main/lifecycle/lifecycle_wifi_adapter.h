#pragma once

#include "esp_err.h"

/**
 * @file lifecycle_wifi_adapter.h
 * @brief Adapter layer between the generic WiFi manager and the lifecycle manager
 * 
 * This adapter provides the integration between the reusable WiFi manager module
 * and the lifecycle-specific configuration and event handling. It is the only
 * module that includes both wifi_manager.h and lifecycle_manager.h, maintaining
 * clean separation of concerns.
 */

/**
 * @brief Initialize the WiFi manager with lifecycle configuration
 * 
 * This function retrieves the AP configuration from the lifecycle manager
 * (SSID, password, hide-when-connected setting) and initializes the WiFi
 * manager with these settings. It also registers a callback to translate
 * WiFi events into lifecycle events.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_wifi_adapter_init(void);

/**
 * @brief Update WiFi manager configuration from lifecycle settings
 * 
 * This function should be called when the lifecycle configuration changes
 * (e.g., when the user updates AP settings). It retrieves the current
 * configuration from the lifecycle manager and applies it to the WiFi manager.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_wifi_adapter_update_config(void);