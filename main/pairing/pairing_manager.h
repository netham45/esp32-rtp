#ifndef PAIRING_MANAGER_H
#define PAIRING_MANAGER_H

#include "esp_err.h"

/**
 * @brief Starts the pairing process.
 *
 * This function will scan for other ESP32 devices in AP mode, connect to the first one found,
 * and send the current device's WiFi credentials to it.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t pairing_manager_start(void);

#endif // PAIRING_MANAGER_H
