#ifndef LIFECYCLE_SERVICES_H
#define LIFECYCLE_SERVICES_H

#include "esp_err.h"

/**
 * @file services.h
 * @brief Service initialization functions for lifecycle manager
 * 
 * This module handles the initialization of network services including
 * WiFi, web server, SPDIF receiver (for sender mode), and mDNS/NTP services.
 */

/**
 * @brief Initialize and start WiFi manager
 * 
 * Initializes the WiFi manager and attempts to connect to the strongest
 * available network. If that fails, falls back to stored credentials or AP mode.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_services_init_wifi(void);

/**
 * @brief Start the web server
 * 
 * Starts the web server for device configuration. Works in both AP and STA modes.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_services_init_web_server(void);

/**
 * @brief Initialize SPDIF receiver for sender mode
 * 
 * Initializes the SPDIF receiver when in sender mode. Only applicable
 * for SPDIF builds when configured as a sender.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_services_init_spdif_receiver(void);

/**
 * @brief Start mDNS and NTP services
 * 
 * Starts mDNS discovery service if enabled in configuration,
 * and initializes NTP client for time synchronization.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_services_init_mdns(void);

#endif // LIFECYCLE_SERVICES_H