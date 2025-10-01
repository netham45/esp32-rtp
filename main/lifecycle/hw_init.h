#ifndef LIFECYCLE_HW_INIT_H
#define LIFECYCLE_HW_INIT_H

#include "esp_err.h"

/**
 * @file hw_init.h
 * @brief Hardware initialization functions for lifecycle manager
 * 
 * This module handles the initialization of hardware components including
 * NVS, configuration manager, OTA manager, battery charger, power management,
 * and DAC detection.
 */

/**
 * @brief Initialize NVS flash
 * 
 * Initializes the non-volatile storage (NVS) flash. If initialization fails
 * due to no free pages or version mismatch, it will erase and reinitialize.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_hw_init_nvs(void);

/**
 * @brief Initialize configuration manager
 * 
 * Initializes the application configuration manager which handles
 * loading and saving device settings.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_hw_init_config(void);

/**
 * @brief Initialize OTA manager
 * 
 * Initializes the OTA (Over-The-Air) update manager, marks current
 * firmware as valid, and clears any leftover OTA state.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_hw_init_ota(void);

/**
 * @brief Initialize BQ25895 battery charger
 * 
 * Initializes the BQ25895 battery charger IC if present.
 * Non-critical - device will continue operation if initialization fails.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_hw_init_battery(void);

/**
 * @brief Configure power management
 * 
 * Configures ESP32 power management settings including CPU frequency
 * scaling and light sleep mode if enabled in menuconfig.
 * 
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if PM not enabled
 */
esp_err_t lifecycle_hw_init_power_management(void);

/**
 * @brief Perform DAC detection logic
 * 
 * Determines if DAC detection should be performed based on device mode.
 * In USB builds with lazy initialization, this may defer USB host init.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_hw_init_dac_detection(void);

#endif // LIFECYCLE_HW_INIT_H