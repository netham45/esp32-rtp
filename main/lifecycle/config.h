#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "../config/config_manager.h"

/**
 * @file config.h
 * @brief Configuration facade for lifecycle manager
 * 
 * This module provides a configuration API that wraps config_manager
 * and handles immediate updates to subsystems when configuration changes.
 */

// Configuration getter functions
uint16_t lifecycle_get_port(void);
const char* lifecycle_get_hostname(void);
uint32_t lifecycle_get_sample_rate(void);
uint8_t lifecycle_get_bit_depth(void);
float lifecycle_get_volume(void);
device_mode_t lifecycle_get_device_mode(void);
bool lifecycle_get_enable_usb_sender(void);
bool lifecycle_get_enable_spdif_sender(void);
const char* lifecycle_get_ap_ssid(void);
const char* lifecycle_get_ap_password(void);
bool lifecycle_get_hide_ap_when_connected(void);
const char* lifecycle_get_sender_destination_ip(void);
uint16_t lifecycle_get_sender_destination_port(void);
uint8_t lifecycle_get_initial_buffer_size(void);
uint8_t lifecycle_get_max_buffer_size(void);
uint8_t lifecycle_get_buffer_grow_step_size(void);
uint8_t lifecycle_get_max_grow_size(void);
uint8_t lifecycle_get_spdif_data_pin(void);
bool lifecycle_get_use_direct_write(void);
uint32_t lifecycle_get_silence_threshold_ms(void);
uint32_t lifecycle_get_network_check_interval_ms(void);
uint8_t lifecycle_get_activity_threshold_packets(void);
uint16_t lifecycle_get_silence_amplitude_threshold(void);
uint32_t lifecycle_get_network_inactivity_timeout_ms(void);
bool lifecycle_get_enable_mdns_discovery(void);
uint32_t lifecycle_get_discovery_interval_ms(void);
bool lifecycle_get_auto_select_best_device(void);

// Configuration setter functions
esp_err_t lifecycle_set_port(uint16_t port);
esp_err_t lifecycle_set_hostname(const char* hostname);
esp_err_t lifecycle_set_volume(float volume);
esp_err_t lifecycle_set_device_mode(device_mode_t mode);
esp_err_t lifecycle_set_enable_usb_sender(bool enable);
esp_err_t lifecycle_set_enable_spdif_sender(bool enable);
esp_err_t lifecycle_set_ap_ssid(const char* ssid);
esp_err_t lifecycle_set_ap_password(const char* password);
esp_err_t lifecycle_set_hide_ap_when_connected(bool hide);
esp_err_t lifecycle_set_sender_destination_ip(const char* ip);
esp_err_t lifecycle_set_sender_destination_port(uint16_t port);
esp_err_t lifecycle_set_initial_buffer_size(uint8_t size);
esp_err_t lifecycle_set_max_buffer_size(uint8_t size);
esp_err_t lifecycle_set_buffer_grow_step_size(uint8_t size);
esp_err_t lifecycle_set_max_grow_size(uint8_t size);
esp_err_t lifecycle_set_spdif_data_pin(uint8_t pin);
esp_err_t lifecycle_set_silence_threshold_ms(uint32_t threshold_ms);
esp_err_t lifecycle_set_network_check_interval_ms(uint32_t interval_ms);
esp_err_t lifecycle_set_activity_threshold_packets(uint8_t packets);
esp_err_t lifecycle_set_silence_amplitude_threshold(uint16_t threshold);
esp_err_t lifecycle_set_network_inactivity_timeout_ms(uint32_t timeout_ms);
esp_err_t lifecycle_set_enable_mdns_discovery(bool enable);
esp_err_t lifecycle_set_discovery_interval_ms(uint32_t interval_ms);
esp_err_t lifecycle_set_auto_select_best_device(bool enable);

// Batch update structure for atomic multi-parameter updates
typedef struct lifecycle_config_update {
    bool update_port;
    uint16_t port;

    bool update_hostname;
    const char* hostname;

    bool update_volume;
    float volume;
    
    bool update_device_mode;
    device_mode_t device_mode;
    
    bool update_enable_usb_sender;  // Legacy compatibility
    bool enable_usb_sender;
    
    bool update_enable_spdif_sender;  // Legacy compatibility
    bool enable_spdif_sender;
    
    bool update_ap_ssid;
    const char* ap_ssid;
    
    bool update_ap_password;
    const char* ap_password;
    
    bool update_hide_ap_when_connected;
    bool hide_ap_when_connected;
    
    bool update_sender_destination_ip;
    const char* sender_destination_ip;
    
    bool update_sender_destination_port;
    uint16_t sender_destination_port;
    
    bool update_initial_buffer_size;
    uint8_t initial_buffer_size;
    
    bool update_max_buffer_size;
    uint8_t max_buffer_size;
    
    bool update_buffer_grow_step_size;
    uint8_t buffer_grow_step_size;
    
    bool update_max_grow_size;
    uint8_t max_grow_size;
    
    bool update_spdif_data_pin;
    uint8_t spdif_data_pin;
    
    bool update_use_direct_write;
    bool use_direct_write;
    
    bool update_silence_threshold_ms;
    uint32_t silence_threshold_ms;
    
    bool update_network_check_interval_ms;
    uint32_t network_check_interval_ms;
    
    bool update_activity_threshold_packets;
    uint8_t activity_threshold_packets;
    
    bool update_silence_amplitude_threshold;
    uint16_t silence_amplitude_threshold;
    
    bool update_network_inactivity_timeout_ms;
    uint32_t network_inactivity_timeout_ms;
    
    bool update_enable_mdns_discovery;
    bool enable_mdns_discovery;
    
    bool update_discovery_interval_ms;
    uint32_t discovery_interval_ms;
    
    bool update_auto_select_best_device;
    bool auto_select_best_device;
    
    bool update_setup_wizard_completed;
    bool setup_wizard_completed;

    bool update_sap_stream_name;
    const char* sap_stream_name;
} lifecycle_config_update_t;

// Batch update function
esp_err_t lifecycle_update_config_batch(const lifecycle_config_update_t* updates);

// Setup wizard functions
bool lifecycle_get_setup_wizard_completed(void);
esp_err_t lifecycle_set_setup_wizard_completed(bool completed);

// Configuration management
esp_err_t lifecycle_reset_config(void);
esp_err_t lifecycle_save_config(void);

// Internal function used by lifecycle_manager for unified config change handling
bool lifecycle_config_handle_configuration_changed(void);