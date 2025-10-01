#ifndef LIFECYCLE_STATE_MACHINE_H
#define LIFECYCLE_STATE_MACHINE_H

#include "esp_err.h"
#include "../lifecycle_manager.h"

/**
 * @file state_machine.h
 * @brief State machine core for lifecycle manager
 * 
 * This module handles the state machine logic including:
 * - State transitions
 * - State entry/exit handlers
 * - Event dispatching
 * - The main lifecycle manager task
 */

/**
 * @brief Initialize the lifecycle state machine
 * 
 * Creates the event queue and starts the lifecycle manager task.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_state_machine_init(void);

/**
 * @brief Post an event to the state machine
 * 
 * Thread-safe function to post an event to the state machine's event queue.
 * 
 * @param event The event to post
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_state_machine_post_event(lifecycle_event_t event);

/**
 * @brief Get the current state of the state machine
 * 
 * @return The current lifecycle state
 */
lifecycle_state_t lifecycle_state_machine_get_current_state(void);

#endif // LIFECYCLE_STATE_MACHINE_H