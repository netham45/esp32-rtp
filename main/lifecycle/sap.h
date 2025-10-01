#ifndef LIFECYCLE_SAP_H
#define LIFECYCLE_SAP_H

#include "esp_err.h"
#include <stdint.h>

/**
 * @file sap.h
 * @brief SAP (Session Announcement Protocol) management for lifecycle manager
 * 
 * This module handles SAP stream announcements and automatic stream configuration.
 */

/**
 * @brief Notify lifecycle manager about a SAP stream announcement
 * 
 * This function is called when a SAP stream announcement is received.
 * If the stream matches the configured stream name, it will automatically
 * configure the network to receive from that stream.
 *
 * @param stream_name The name of the stream
 * @param destination_ip The destination IP (multicast or unicast)
 * @param source_ip The source IP of the announcement
 * @param port The port number
 * @param sample_rate The sample rate
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_manager_notify_sap_stream(const char* stream_name,
                                               const char* destination_ip,
                                               const char* source_ip,
                                               uint16_t port,
                                               uint32_t sample_rate);

/**
 * @brief Get the SAP stream name to automatically connect to
 * @return Pointer to the SAP stream name string
 */
const char* lifecycle_get_sap_stream_name(void);

/**
 * @brief Set the SAP stream name to automatically connect to
 * @param stream_name The SAP stream name (empty string to disable auto-connect)
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_sap_stream_name(const char* stream_name);

#endif // LIFECYCLE_SAP_H