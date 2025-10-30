/**
 * @file bq25895_integration.h
 * @brief Header file for BQ25895 integration
 */

#ifndef BQ25895_INTEGRATION_H
#define BQ25895_INTEGRATION_H

#include "esp_err.h"
#include "bq25895.h" // Include the main driver header for status/params structs

/**
 * @brief Initialize the BQ25895 battery charger and its web interface.
 *
 * This function initializes the I2C communication, the BQ25895 driver,
 * and the associated web interface components.
 *
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t bq25895_integration_init(void);

/**
 * @brief Get the current status of the BQ25895 charger.
 *
 * @param status Pointer to a structure where the status will be stored.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t bq25895_integration_get_status(bq25895_status_t *status);

/**
 * @brief Get the current charge parameters of the BQ25895 charger.
 *
 * @param params Pointer to a structure where the parameters will be stored.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t bq25895_integration_get_charge_params(bq25895_charge_params_t *params);

/**
 * @brief Set the charge parameters for the BQ25895 charger.
 *
 * @param params Pointer to a structure containing the parameters to set.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t bq25895_integration_set_charge_params(const bq25895_charge_params_t *params);

/**
 * @brief Reset the BQ25895 device to its default register values.
 *
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t bq25895_integration_reset(void);

/**
 * @brief Set the CE pin (IO12) state
 * 
 * @param enable If true, set CE pin high; if false, set CE pin low
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t bq25895_integration_set_ce_pin(bool enable);

/**
 * @brief Read a BQ25895 register through the integration layer.
 */
esp_err_t bq25895_integration_read_register(uint8_t reg, uint8_t *value);

/**
 * @brief Write a BQ25895 register through the integration layer.
 */
esp_err_t bq25895_integration_write_register(uint8_t reg, uint8_t value);

/**
 * @brief Run periodic maintenance actions for the BQ25895 integration.
 *
 * Call this cooperatively to service the charger watchdog without allocating a
 * dedicated FreeRTOS task.
 */
void bq25895_integration_tick(void);

#endif // BQ25895_INTEGRATION_H
