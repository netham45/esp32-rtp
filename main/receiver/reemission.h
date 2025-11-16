#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the reemission subsystem. Safe to call multiple times.
 */
esp_err_t reemission_init(void);

/**
 * @brief Start timer-driven reemission at the current audio parameters.
 */
esp_err_t reemission_start(void);

/**
 * @brief Stop periodic reemission ticks.
 */
void reemission_stop(void);

/**
 * @brief Recompute timer period using the latest audio configuration.
 */
esp_err_t reemission_update_audio_params(void);
