#pragma once
#include "esp_err.h"
#include "bq25895/bq25895.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build a JSON object representing the current BQ25895 status.
 *
 * @param status Pointer to populated status structure.
 * @return Pointer to a cJSON object on success, NULL on allocation failure or invalid args.
 *         Caller takes ownership and must call cJSON_Delete().
 */
cJSON *bq25895_status_to_json(const bq25895_status_t *status);

/**
 * @brief Build a JSON object representing the current BQ25895 charge parameters.
 *
 * @param params Pointer to populated charge parameter structure.
 * @return Pointer to a cJSON object on success, NULL on allocation failure or invalid args.
 *         Caller takes ownership and must call cJSON_Delete().
 */
cJSON *bq25895_params_to_json(const bq25895_charge_params_t *params);

/**
 * @brief Apply values from a JSON object onto a charge parameter structure.
 *
 * @param params Pointer to structure to update in place.
 * @param json JSON object containing any subset of writable fields.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG when input is malformed.
 */
esp_err_t bq25895_params_update_from_json(bq25895_charge_params_t *params, const cJSON *json);

#ifdef __cplusplus
}
#endif