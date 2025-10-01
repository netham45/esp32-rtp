#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "config/config_manager.h"  // For device_mode_t

/**
 * @brief Events that can be sent to the lifecycle manager
 */
typedef enum {
    LIFECYCLE_EVENT_WIFI_CONNECTED,
    LIFECYCLE_EVENT_WIFI_DISCONNECTED,
    LIFECYCLE_EVENT_USB_DAC_CONNECTED,
    LIFECYCLE_EVENT_USB_DAC_DISCONNECTED,
    LIFECYCLE_EVENT_CONFIGURATION_CHANGED,
    LIFECYCLE_EVENT_ENTER_SLEEP,
    LIFECYCLE_EVENT_WAKE_UP,
    LIFECYCLE_EVENT_START_PAIRING,
    LIFECYCLE_EVENT_PAIRING_COMPLETE,
    LIFECYCLE_EVENT_CANCEL_PAIRING,
    LIFECYCLE_EVENT_MDNS_DEVICE_FOUND,      // mDNS device discovered
    LIFECYCLE_EVENT_MDNS_DEVICE_LOST,       // mDNS device lost
    LIFECYCLE_EVENT_SAMPLE_RATE_CHANGE,     // Sample rate change requested
    LIFECYCLE_EVENT_SAP_STREAM_FOUND        // SAP stream announcement received
} lifecycle_event_t;

// Define the possible states of the lifecycle manager
typedef enum {
    LIFECYCLE_STATE_INITIALIZING,
    LIFECYCLE_STATE_HW_INIT,
    LIFECYCLE_STATE_STARTING_SERVICES,
    LIFECYCLE_STATE_AWAITING_MODE_CONFIG,
    LIFECYCLE_STATE_MODE_SENDER_USB,
    LIFECYCLE_STATE_MODE_SENDER_SPDIF,
    LIFECYCLE_STATE_MODE_RECEIVER_USB,
    LIFECYCLE_STATE_MODE_RECEIVER_SPDIF,
    LIFECYCLE_STATE_PAIRING,
    LIFECYCLE_STATE_SLEEPING,
    LIFECYCLE_STATE_ERROR
} lifecycle_state_t;

/**
 * @brief Initialize and start the lifecycle manager.
 *
 * This function creates the lifecycle manager task and event queue.
 * It should be called once from app_main.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t lifecycle_manager_init(void);

/**
 * @brief Post an event to the lifecycle manager's event queue.
 *
 * This function is thread-safe and can be called from any task or ISR.
 *
 * @param event The event to post.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t lifecycle_manager_post_event(lifecycle_event_t event);

/**
 * @brief Posts an event to change the sample rate.
 *
 * @param new_rate The new sample rate.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t lifecycle_manager_change_sample_rate(uint32_t new_rate);

/**
 * @brief Notify lifecycle manager about a SAP stream announcement.
 *
 * @param stream_name The name of the stream
 * @param destination_ip The destination IP (multicast or unicast)
 * @param source_ip The source IP of the announcement
 * @param port The port number
 * @param sample_rate The sample rate
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t lifecycle_manager_notify_sap_stream(const char* stream_name,
                                              const char* destination_ip,
                                              const char* source_ip,
                                              uint16_t port,
                                              uint32_t sample_rate);

/**
 * @brief Report network activity to the lifecycle manager.
 *
 * This function is called by the network input module when a packet is received.
 * It is used to wake the device from sleep.
 */
void lifecycle_manager_report_network_activity(void);

// Configuration getter functions
/**
 * @brief Get the configured port number
 * @return The port number
 */
uint16_t lifecycle_get_port(void);

/**
 * @brief Get the configured hostname
 * @return Pointer to the hostname string
 */
const char* lifecycle_get_hostname(void);

/**
 * @brief Get the configured sample rate
 * @return The sample rate in Hz
 */
uint32_t lifecycle_get_sample_rate(void);

/**
 * @brief Get the configured bit depth
 * @return The bit depth in bits
 */
uint8_t lifecycle_get_bit_depth(void);

/**
 * @brief Get the configured volume
 * @return The volume level
 */
float lifecycle_get_volume(void);

/**
 * @brief Get the current device mode
 * @return The current device mode
 */
device_mode_t lifecycle_get_device_mode(void);

/**
 * @brief Get whether USB sender mode is enabled (legacy compatibility)
 * @return true if USB sender is enabled, false otherwise
 */
bool lifecycle_get_enable_usb_sender(void);

/**
 * @brief Get whether SPDIF sender mode is enabled
 * @return true if SPDIF sender is enabled, false otherwise
 */
bool lifecycle_get_enable_spdif_sender(void);

/**
 * @brief Get the AP SSID
 * @return Pointer to the AP SSID string
 */
const char* lifecycle_get_ap_ssid(void);

/**
 * @brief Get the AP password
 * @return Pointer to the AP password string
 */
const char* lifecycle_get_ap_password(void);

/**
 * @brief Get whether to hide AP when connected
 * @return true if AP should be hidden when connected, false otherwise
 */
bool lifecycle_get_hide_ap_when_connected(void);

/**
 * @brief Get the sender destination IP address
 * @return Pointer to the destination IP string
 */
const char* lifecycle_get_sender_destination_ip(void);

/**
 * @brief Get the sender destination port
 * @return The destination port number
 */
uint16_t lifecycle_get_sender_destination_port(void);

/**
 * @brief Get the initial buffer size
 * @return The initial buffer size
 */
uint8_t lifecycle_get_initial_buffer_size(void);

/**
 * @brief Get the maximum buffer size
 * @return The maximum buffer size
 */
uint8_t lifecycle_get_max_buffer_size(void);

/**
 * @brief Get the buffer grow step size
 * @return The buffer grow step size
 */
uint8_t lifecycle_get_buffer_grow_step_size(void);

/**
 * @brief Get the maximum grow size
 * @return The maximum grow size
 */
uint8_t lifecycle_get_max_grow_size(void);

/**
 * @brief Get the SPDIF data pin number
 * @return The SPDIF data pin number
 */
uint8_t lifecycle_get_spdif_data_pin(void);

/**
 * @brief Get whether to use direct write mode
 * @return true if direct write is enabled, false otherwise
 */
bool lifecycle_get_use_direct_write(void);

/**
 * @brief Get the silence threshold in milliseconds
 * @return The silence threshold in ms
 */
uint32_t lifecycle_get_silence_threshold_ms(void);

/**
 * @brief Get the network check interval in milliseconds
 * @return The network check interval in ms
 */
uint32_t lifecycle_get_network_check_interval_ms(void);

/**
 * @brief Get the activity threshold in packets
 * @return The activity threshold packet count
 */
uint8_t lifecycle_get_activity_threshold_packets(void);

/**
 * @brief Get the silence amplitude threshold
 * @return The silence amplitude threshold
 */
uint16_t lifecycle_get_silence_amplitude_threshold(void);

/**
 * @brief Get the network inactivity timeout in milliseconds
 * @return The network inactivity timeout in ms
 */
uint32_t lifecycle_get_network_inactivity_timeout_ms(void);

/**
 * @brief Get whether mDNS discovery is enabled
 * @return true if mDNS discovery is enabled, false otherwise
 */
bool lifecycle_get_enable_mdns_discovery(void);

/**
 * @brief Get the discovery interval in milliseconds
 * @return The discovery interval in ms
 */
uint32_t lifecycle_get_discovery_interval_ms(void);

/**
 * @brief Get whether to auto-select the best device
 * @return true if auto-select is enabled, false otherwise
 */
bool lifecycle_get_auto_select_best_device(void);

// Configuration setter functions
/**
 * @brief Set the port number
 * @param port The new port number
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_port(uint16_t port);

/**
 * @brief Set the hostname
 * @param hostname The new hostname string
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_hostname(const char* hostname);

/**
 * @brief Set the volume level
 * @param volume The new volume level (0.0 to 1.0)
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_volume(float volume);

/**
 * @brief Set the device mode
 * @param mode The new device mode
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_device_mode(device_mode_t mode);

/**
 * @brief Set whether USB sender mode is enabled (legacy compatibility)
 * @param enable true to enable USB sender, false to disable
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_enable_usb_sender(bool enable);

/**
 * @brief Set whether SPDIF sender mode is enabled
 * @param enable true to enable SPDIF sender, false to disable
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_enable_spdif_sender(bool enable);

/**
 * @brief Set the AP SSID
 * @param ssid The new AP SSID string
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_ap_ssid(const char* ssid);

/**
 * @brief Set the AP password
 * @param password The new AP password string
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_ap_password(const char* password);

/**
 * @brief Set whether to hide AP when connected to WiFi
 * @param hide true to hide AP when connected, false to keep it visible
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_hide_ap_when_connected(bool hide);

/**
 * @brief Set the sender destination IP address
 * @param ip The new destination IP string
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_sender_destination_ip(const char* ip);

/**
 * @brief Set the sender destination port
 * @param port The new destination port number
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_sender_destination_port(uint16_t port);

/**
 * @brief Set the initial buffer size
 * @param size The new initial buffer size
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_initial_buffer_size(uint8_t size);

/**
 * @brief Set the maximum buffer size
 * @param size The new maximum buffer size
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_max_buffer_size(uint8_t size);

/**
 * @brief Set the buffer grow step size
 * @param size The new buffer grow step size
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_buffer_grow_step_size(uint8_t size);

/**
 * @brief Set the maximum grow size
 * @param size The new maximum grow size
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_max_grow_size(uint8_t size);

/**
 * @brief Set the SPDIF data pin number
 * @param pin The new SPDIF data pin number
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_spdif_data_pin(uint8_t pin);

/**
 * @brief Set the silence threshold in milliseconds
 * @param threshold_ms The new silence threshold in ms
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_silence_threshold_ms(uint32_t threshold_ms);

/**
 * @brief Set the network check interval in milliseconds
 * @param interval_ms The new network check interval in ms
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_network_check_interval_ms(uint32_t interval_ms);

/**
 * @brief Set the activity threshold in packets
 * @param packets The new activity threshold packet count
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_activity_threshold_packets(uint8_t packets);

/**
 * @brief Set the silence amplitude threshold
 * @param threshold The new silence amplitude threshold
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_silence_amplitude_threshold(uint16_t threshold);

/**
 * @brief Set the network inactivity timeout in milliseconds
 * @param timeout_ms The new network inactivity timeout in ms
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_network_inactivity_timeout_ms(uint32_t timeout_ms);

/**
 * @brief Set whether mDNS discovery is enabled
 * @param enable true to enable mDNS discovery, false to disable
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_enable_mdns_discovery(bool enable);

/**
 * @brief Set the discovery interval in milliseconds
 * @param interval_ms The new discovery interval in ms
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_discovery_interval_ms(uint32_t interval_ms);

/**
 * @brief Set whether to auto-select the best device
 * @param enable true to enable auto-select, false to disable
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_auto_select_best_device(bool enable);

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

/**
 * @brief Configuration update structure for batch updates
 * (Full definition in lifecycle/config.h)
 */
typedef struct lifecycle_config_update lifecycle_config_update_t;

/**
 * @brief Update multiple configuration settings at once
 * @param updates Pointer to the configuration update structure
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_update_config_batch(const lifecycle_config_update_t* updates);

/**
 * @brief Get whether the setup wizard has been completed
 * @return true if setup wizard is completed, false otherwise
 */
bool lifecycle_get_setup_wizard_completed(void);

/**
 * @brief Set the setup wizard completion status
 * @param completed true if setup wizard is completed, false otherwise
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_set_setup_wizard_completed(bool completed);

/**
 * @brief Reset the configuration to factory defaults
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_reset_config(void);

/**
 * @brief Save the current configuration to persistent storage
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t lifecycle_save_config(void);