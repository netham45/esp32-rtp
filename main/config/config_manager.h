#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifi/wifi_manager.h" // For WIFI_PASSWORD_MAX_LENGTH

// NVS namespace for storing configuration
#define CONFIG_NVS_NAMESPACE "app_config"

// Device mode enumeration
typedef enum {
    MODE_RECEIVER_USB = 0,    // Receive audio from network and output via USB
    MODE_RECEIVER_SPDIF,      // Receive audio from network and output via SPDIF
    MODE_SENDER_USB,          // Capture audio via USB and stream to network
    MODE_SENDER_SPDIF,        // Capture audio via SPDIF and stream to network
} device_mode_t;

typedef struct {
    // Network
    uint16_t port;
    char hostname[64];                                  // Device hostname
    
    // WiFi AP configuration
    char ap_ssid[WIFI_SSID_MAX_LENGTH + 1];         // AP mode SSID
    char ap_password[WIFI_PASSWORD_MAX_LENGTH + 1]; // AP mode password (empty for open network)
    bool hide_ap_when_connected;                    // Hide AP when connected to WiFi
    
    // Buffer configuration
    uint8_t initial_buffer_size;
    uint8_t buffer_grow_step_size;
    uint8_t max_buffer_size;
    uint8_t max_grow_size;
    
    // Audio configuration
    uint32_t sample_rate;
    uint8_t bit_depth;
    float volume;
    uint8_t spdif_data_pin;  // Only used when IS_SPDIF is defined
    
    // Sleep configuration
    uint32_t silence_threshold_ms;
    uint32_t network_check_interval_ms;
    uint8_t activity_threshold_packets;
    uint16_t silence_amplitude_threshold;
    uint32_t network_inactivity_timeout_ms;
    
    // Device mode configuration
    device_mode_t device_mode;             // Device operational mode
    bool enable_usb_sender;                // Legacy: Enable USB sender mode (deprecated, use device_mode)
    bool enable_spdif_sender;              // Legacy: Enable SPDIF sender mode (deprecated, use device_mode)
    char sender_destination_ip[16];        // Destination IP for audio packets (sender modes only)
    uint16_t sender_destination_port;      // Destination port for audio packets
    
    // AP-Only mode configuration
    bool ap_only_mode;                     // Enable AP-Only mode (no WiFi client connection)
    
    // Audio processing configuration
    bool use_direct_write;                 // Use direct write instead of buffering
    
    // mDNS discovery configuration
    bool enable_mdns_discovery;            // Enable mDNS discovery of Scream devices
    uint32_t discovery_interval_ms;        // Discovery scan interval in milliseconds
    bool auto_select_best_device;          // Automatically select best available device
    uint32_t mdns_discovery_interval_ms;   // mDNS discovery interval in milliseconds (default 10000)
    
    // Setup wizard status
    bool setup_wizard_completed;           // Whether the setup wizard has been completed
} app_config_t;

// Initialize configuration (load from NVS or use defaults)
esp_err_t config_manager_init(void);

// Get current configuration
app_config_t* config_manager_get_config(void);

// Save configuration to NVS
esp_err_t config_manager_save_config(void);

// Save specific configuration to NVS
esp_err_t config_manager_save_setting(const char* key, void* value, size_t size);

// Reset configuration to defaults
esp_err_t config_manager_reset(void);

// Reload configuration from NVS
esp_err_t config_manager_reload(void);
