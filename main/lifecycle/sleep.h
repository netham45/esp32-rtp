#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @file sleep.h
 * @brief Sleep and power management for lifecycle manager
 * 
 * This module handles silence detection, network activity monitoring,
 * and power management for receiver modes.
 */

/**
 * @brief Report network activity to the sleep manager
 * 
 * Called by the network input module when a packet is received.
 * Used to wake the device from sleep or prevent sleep mode entry.
 */
void lifecycle_sleep_report_network_activity(void);

/**
 * @brief Enter silence sleep mode
 * 
 * Configures the device for low-power operation while monitoring
 * for network activity to wake up.
 */
void lifecycle_sleep_enter_silence_mode(void);

/**
 * @brief Exit silence sleep mode
 * 
 * Restores normal operation after waking from sleep.
 */
void lifecycle_sleep_exit_silence_mode(void);

/**
 * @brief Update sleep monitoring parameters from configuration
 * 
 * Called when sleep-related configuration parameters change.
 * Updates cached values used by the monitoring task.
 */
void lifecycle_sleep_update_params(void);