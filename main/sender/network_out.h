#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Initialize the RTP sender functionality
 * This sets up the necessary components but doesn't start sending
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t rtp_sender_init(void);

/**
 * Start the RTP sender
 * This begins capture from USB audio and sending to the network
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t rtp_sender_start(void);

/**
 * Stop the RTP sender
 * This halts capture and network sending
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t rtp_sender_stop(void);

/**
 * Check if the RTP sender is currently running
 *
 * @return true if running, false otherwise
 */
bool rtp_sender_is_running(void);

/**
 * Set the mute state for the RTP sender
 *
 * @param mute true to mute, false to unmute
 */
void rtp_sender_set_mute(bool mute);

/**
 * Set the volume for the RTP sender (0-100)
 *
 * @param volume Volume level (0-100)
 */
void rtp_sender_set_volume(uint32_t volume);

/**
 * Update the destination IP and port for the RTP sender
 * This can be called while the sender is running to change the destination
 *
 * @return ESP_OK on success, or an error code on failure
 */

/**
 * Auto-select the best available Scream device
 * This function scans discovered mDNS devices and selects the most suitable receiver
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no suitable device, or other error codes
 */
esp_err_t rtp_sender_auto_select_device(void);
esp_err_t rtp_sender_update_destination(void);
