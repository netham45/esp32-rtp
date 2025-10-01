#ifndef LIFECYCLE_MODES_H
#define LIFECYCLE_MODES_H

#include "esp_err.h"
#include "../lifecycle_manager.h"

/**
 * @file modes.h
 * @brief Mode start/stop controllers for lifecycle manager
 * 
 * This module handles starting and stopping different operational modes:
 * - USB sender mode
 * - SPDIF sender mode
 * - USB receiver mode
 * - SPDIF receiver mode
 */

/**
 * @brief Start the specified operational mode
 * 
 * Maps the lifecycle state to the appropriate mode start function
 * and initializes all necessary components for that mode.
 * 
 * @param mode The mode to start
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_mode_start(lifecycle_state_t mode);

/**
 * @brief Stop the specified operational mode
 * 
 * Maps the lifecycle state to the appropriate mode stop function
 * and cleans up all resources associated with that mode.
 * 
 * @param mode The mode to stop
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_mode_stop(lifecycle_state_t mode);

#endif // LIFECYCLE_MODES_H