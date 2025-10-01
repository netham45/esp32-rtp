#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "../lifecycle_manager.h"

/**
 * @file lifecycle_internal.h
 * @brief Internal structures and context shared across lifecycle modules
 * 
 * This header defines the internal state and context used by the lifecycle
 * manager and its submodules. It is not part of the public API.
 */

/**
 * @brief Lifecycle manager internal context
 * 
 * This structure holds the shared state for the lifecycle manager,
 * including event groups, monitoring state, and cached configuration values.
 */
typedef struct {
    // Event group for network activity monitoring
    EventGroupHandle_t network_activity_event_group;
    
    // Network monitoring state
    TaskHandle_t network_monitor_task_handle;
    volatile uint32_t packet_counter;
    volatile bool monitoring_active;
    volatile TickType_t last_packet_time;
    
    // Cached values for sleep monitoring (thread-safe local copies)
    volatile uint32_t cached_silence_threshold_ms;
    volatile uint32_t cached_network_check_interval_ms;
    volatile uint8_t cached_activity_threshold_packets;
    volatile uint16_t cached_silence_amplitude_threshold;
    volatile uint32_t cached_network_inactivity_timeout_ms;
    
    // Current lifecycle state
    lifecycle_state_t current_state;
} lifecycle_context_t;

/**
 * @brief Get the lifecycle manager context
 * 
 * Returns a pointer to the shared lifecycle context. This is used by
 * submodules to access shared state.
 * 
 * @return Pointer to the lifecycle context
 */
lifecycle_context_t* lifecycle_get_context(void);

/**
 * @brief Get the current lifecycle state
 * 
 * @return The current lifecycle state
 */
lifecycle_state_t lifecycle_get_current_state(void);

/**
 * @brief Get the network activity event group
 * 
 * @return Handle to the network activity event group
 */
EventGroupHandle_t lifecycle_get_network_event_group(void);
