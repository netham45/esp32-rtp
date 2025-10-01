#ifndef LIFECYCLE_RECONFIG_H
#define LIFECYCLE_RECONFIG_H

#include "esp_err.h"
#include <stdint.h>

/**
 * @file reconfig.h
 * @brief Runtime reconfiguration functions for lifecycle manager
 * 
 * This module handles runtime reconfiguration of audio parameters without
 * requiring a system restart. Includes sample rate, SPDIF pin, and buffer
 * parameter reconfiguration.
 */

/**
 * @brief Reconfigure sample rate for active receiver modes without restart
 * 
 * This function temporarily pauses audio output, reconfigures the appropriate
 * subsystem (USB or SPDIF), and resumes playback.
 * 
 * @param new_rate The new sample rate in Hz
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not in receiver mode,
 *         or other error code on failure
 */
esp_err_t lifecycle_reconfig_sample_rate(uint32_t new_rate);

/**
 * @brief Reconfigure SPDIF pin for active SPDIF modes without restart
 * 
 * This function handles runtime reconfiguration of the SPDIF data pin
 * for both sender and receiver modes.
 * 
 * @param new_pin The new SPDIF data pin (GPIO number)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not in SPDIF mode,
 *         or other error code on failure
 */
esp_err_t lifecycle_reconfig_spdif_pin(uint8_t new_pin);

/**
 * @brief Reconfigure buffer sizes for active receiver modes without restart
 * 
 * This function handles runtime reconfiguration of buffer parameters including
 * initial buffer size and max buffer size. For significant changes like
 * max_buffer_size, this requires reallocating the buffer.
 * 
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not in receiver mode,
 *         or other error code on failure
 */
esp_err_t lifecycle_reconfig_buffer_params(void);

/**
 * @brief Change sample rate and save to configuration
 * 
 * Public API for changing sample rate. Saves the new rate to configuration
 * and attempts immediate reconfiguration if in an active receiver mode.
 * 
 * @param new_rate The new sample rate in Hz
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_manager_change_sample_rate(uint32_t new_rate);

#endif // LIFECYCLE_RECONFIG_H