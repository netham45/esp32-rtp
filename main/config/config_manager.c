#include "config_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "visualizer_task.h"

// Store the active configuration
static app_config_t s_app_config;

// NVS keys for different config parameters
#define NVS_KEY_PORT "port"
#define NVS_KEY_HOSTNAME "hostname"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASSWORD "ap_password"
#define NVS_KEY_HIDE_AP_CONNECTED "hide_ap_conn"
#define NVS_KEY_INIT_BUF_SIZE "init_buf_sz"
#define NVS_KEY_BUF_GROW_STEP "buf_grow_step"
#define NVS_KEY_MAX_BUF_SIZE "max_buf_sz"
#define NVS_KEY_MAX_GROW_SIZE "max_grow_sz"
#define NVS_KEY_SAMPLE_RATE "sample_rate"
#define NVS_KEY_BIT_DEPTH "bit_depth"
#define NVS_KEY_VOLUME "volume"
#define NVS_KEY_SPDIF_DATA_PIN "spdif_pin"
#define NVS_KEY_SILENCE_THRES_MS "silence_ms"
#define NVS_KEY_NET_CHECK_MS "net_check_ms"
#define NVS_KEY_ACTIVITY_PACKETS "act_packets"
#define NVS_KEY_SILENCE_AMPLT "silence_amp"
#define NVS_KEY_NET_INACT_MS "net_inact_ms"
// USB Scream Sender keys
#define NVS_KEY_ENABLE_USB_SENDER "usb_sender"
#define NVS_KEY_SENDER_DEST_IP "sender_ip"
#define NVS_KEY_SENDER_DEST_PORT "sender_port"

// S/PDIF Scream Sender key
#define NVS_KEY_ENABLE_SPDIF_SENDER "spdif_sender"

// Device mode key (new enum-based configuration)
#define NVS_KEY_DEVICE_MODE "device_mode"

// AP-Only mode key
#define NVS_KEY_AP_ONLY_MODE "ap_only_mode"

// Audio processing keys
#define NVS_KEY_USE_DIRECT_WRITE "direct_write"

// mDNS discovery keys
#define NVS_KEY_ENABLE_MDNS_DISCOVERY "mdns_discovery"
#define NVS_KEY_DISCOVERY_INTERVAL_MS "disc_interval"
#define NVS_KEY_AUTO_SELECT_DEVICE "auto_select"

// Setup wizard keys
#define NVS_KEY_SETUP_WIZARD_COMPLETED "wizard_done"

// SAP keys
#define NVS_KEY_SAP_STREAM_NAME "sap_stream"

/**
 * Initialize with default values from config.h
 */
static void set_default_config(void) {
    s_app_config.port = PORT;

    // Generate hostname and AP SSID with MAC suffix
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        // Fall back to base MAC if WiFi isn't initialized
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            // Use default without MAC if we can't get it
            strcpy(s_app_config.hostname, "ESP32-Scream");
            strcpy(s_app_config.ap_ssid, "ESP32-Scream");
            ESP_LOGW(TAG, "Failed to get MAC for hostname/AP SSID, using defaults");
        } else {
            // Format hostname and AP SSID with last 6 hex chars of MAC
            snprintf(s_app_config.hostname, sizeof(s_app_config.hostname),
                    "ESP32-Scream-%02X%02X%02X",
                    mac[3], mac[4], mac[5]);
            snprintf(s_app_config.ap_ssid, sizeof(s_app_config.ap_ssid),
                    "ESP32-Scream-%02X%02X%02X%02X%02X%02X",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            ESP_LOGI(TAG, "Generated hostname: %s", s_app_config.hostname);
            ESP_LOGI(TAG, "Generated AP SSID: %s", s_app_config.ap_ssid);
        }
    } else {
        // Format hostname and AP SSID with last 6 hex chars of MAC
        snprintf(s_app_config.hostname, sizeof(s_app_config.hostname),
                "ESP32-Scream-%02X%02X%02X",
                mac[3], mac[4], mac[5]);
        snprintf(s_app_config.ap_ssid, sizeof(s_app_config.ap_ssid),
                "ESP32-Scream-%02X%02X%02X%02X%02X%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Generated hostname: %s", s_app_config.hostname);
        ESP_LOGI(TAG, "Generated AP SSID: %s", s_app_config.ap_ssid);
    }
    
    s_app_config.ap_password[0] = '\0'; // Default AP password is empty (open network)
    s_app_config.hide_ap_when_connected = true; // Hide AP when connected to WiFi by default
    s_app_config.ap_only_mode = false; // Default to normal mode (not AP-only)
    s_app_config.initial_buffer_size = INITIAL_BUFFER_SIZE;
    s_app_config.buffer_grow_step_size = BUFFER_GROW_STEP_SIZE;
    s_app_config.max_buffer_size = MAX_BUFFER_SIZE;
    s_app_config.max_grow_size = MAX_GROW_SIZE;
    s_app_config.sample_rate = SAMPLE_RATE;
    s_app_config.bit_depth = BIT_DEPTH;
    s_app_config.volume = VOLUME;
    s_app_config.spdif_data_pin = 17; // Default SPDIF pin
    s_app_config.silence_threshold_ms = SILENCE_THRESHOLD_MS;
    s_app_config.network_check_interval_ms = NETWORK_CHECK_INTERVAL_MS;
    s_app_config.activity_threshold_packets = ACTIVITY_THRESHOLD_PACKETS;
    s_app_config.silence_amplitude_threshold = SILENCE_AMPLITUDE_THRESHOLD;
    s_app_config.network_inactivity_timeout_ms = NETWORK_INACTIVITY_TIMEOUT_MS;
    
    // USB Scream Sender defaults
    s_app_config.enable_usb_sender = false;
    s_app_config.enable_spdif_sender = false; // Default S/PDIF sender to disabled
    strcpy(s_app_config.sender_destination_ip, "192.168.1.255"); // Default to broadcast
    s_app_config.sender_destination_port = 40000; // Default ScreamRouter RTP port
    
    // Audio processing defaults
    s_app_config.use_direct_write = true; // Default to direct write mode
    
    // mDNS discovery defaults
    s_app_config.enable_mdns_discovery = true;       // Enable mDNS discovery by default
    s_app_config.discovery_interval_ms = 30000;      // 30 seconds default interval
    s_app_config.auto_select_best_device = false;    // Manual selection by default
    
    // Setup wizard defaults
    s_app_config.setup_wizard_completed = false;     // Wizard not completed by default

    // SAP defaults
    s_app_config.sap_stream_name[0] = '\0';          // No stream selected by default

    // Device mode defaults based on build type
    #ifdef IS_USB
    s_app_config.device_mode = MODE_RECEIVER_USB;
    #elif defined(IS_SPDIF)
    s_app_config.device_mode = MODE_RECEIVER_SPDIF;
    #else
    s_app_config.device_mode = MODE_RECEIVER_USB; // Default fallback
    #endif
}

/**
 * Initialize configuration manager and load settings
 */
esp_err_t config_manager_init(void) {
    ESP_LOGI(TAG, "Initializing configuration manager");
    
    // Set default values first
    set_default_config();
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved configuration found, using defaults");
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Read network settings
    uint16_t port;
    err = nvs_get_u16(nvs_handle, NVS_KEY_PORT, &port);
    if (err == ESP_OK) {
        s_app_config.port = port;
    }

    // Read hostname
    size_t hostname_len = sizeof(s_app_config.hostname);
    err = nvs_get_str(nvs_handle, NVS_KEY_HOSTNAME, s_app_config.hostname, &hostname_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading hostname: %s", esp_err_to_name(err));
    }
    
    // Read AP SSID
    size_t ssid_len = WIFI_SSID_MAX_LENGTH;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, s_app_config.ap_ssid, &ssid_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading AP SSID: %s", esp_err_to_name(err));
    }
    
    // Read AP password
    size_t len = WIFI_PASSWORD_MAX_LENGTH;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASSWORD, s_app_config.ap_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading AP password: %s", esp_err_to_name(err));
    }
    
    // Read hide AP when connected setting
    uint8_t hide_ap;
    err = nvs_get_u8(nvs_handle, NVS_KEY_HIDE_AP_CONNECTED, &hide_ap);
    if (err == ESP_OK) {
        s_app_config.hide_ap_when_connected = (bool)hide_ap;
    }
    
    // Read AP-only mode setting
    uint8_t ap_only;
    err = nvs_get_u8(nvs_handle, NVS_KEY_AP_ONLY_MODE, &ap_only);
    if (err == ESP_OK) {
        s_app_config.ap_only_mode = (bool)ap_only;
    }
    
    // Read buffer settings
    uint8_t u8_value;
    err = nvs_get_u8(nvs_handle, NVS_KEY_INIT_BUF_SIZE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.initial_buffer_size = u8_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_BUF_GROW_STEP, &u8_value);
    if (err == ESP_OK) {
        s_app_config.buffer_grow_step_size = u8_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_MAX_BUF_SIZE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.max_buffer_size = u8_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_MAX_GROW_SIZE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.max_grow_size = u8_value;
    }
    
    // Read audio settings
    uint32_t sample_rate;
    err = nvs_get_u32(nvs_handle, NVS_KEY_SAMPLE_RATE, &sample_rate);
    if (err == ESP_OK) {
        s_app_config.sample_rate = sample_rate;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_BIT_DEPTH, &u8_value);
    if (err == ESP_OK) {
        s_app_config.bit_depth = u8_value;
    }
    
    // Read volume as u32 (stored as integer representation of float * 100)
    uint32_t volume_int;
    err = nvs_get_u32(nvs_handle, NVS_KEY_VOLUME, &volume_int);
    if (err == ESP_OK) {
        s_app_config.volume = (float)volume_int / 100.0f;
    }
    
    // Read SPDIF data pin
    err = nvs_get_u8(nvs_handle, NVS_KEY_SPDIF_DATA_PIN, &u8_value);
    if (err == ESP_OK) {
        s_app_config.spdif_data_pin = u8_value;
        ESP_LOGI(TAG, "Loaded SPDIF data pin: %d", s_app_config.spdif_data_pin);
    }
    
    // Read sleep settings
    uint32_t u32_value;
    err = nvs_get_u32(nvs_handle, NVS_KEY_SILENCE_THRES_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.silence_threshold_ms = u32_value;
    }
    
    err = nvs_get_u32(nvs_handle, NVS_KEY_NET_CHECK_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.network_check_interval_ms = u32_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_ACTIVITY_PACKETS, &u8_value);
    if (err == ESP_OK) {
        s_app_config.activity_threshold_packets = u8_value;
    }
    
    uint16_t u16_value;
    err = nvs_get_u16(nvs_handle, NVS_KEY_SILENCE_AMPLT, &u16_value);
    if (err == ESP_OK) {
        s_app_config.silence_amplitude_threshold = u16_value;
    }
    
    err = nvs_get_u32(nvs_handle, NVS_KEY_NET_INACT_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.network_inactivity_timeout_ms = u32_value;
    }
    
    // Read USB Scream Sender settings
    err = nvs_get_u8(nvs_handle, NVS_KEY_ENABLE_USB_SENDER, &u8_value);
    if (err == ESP_OK) {
        s_app_config.enable_usb_sender = (bool)u8_value;
    }
    
    // Read S/PDIF Scream Sender setting
    err = nvs_get_u8(nvs_handle, NVS_KEY_ENABLE_SPDIF_SENDER, &u8_value);
    if (err == ESP_OK) {
        s_app_config.enable_spdif_sender = (bool)u8_value;
        ESP_LOGI(TAG, "Loaded enable_spdif_sender: %d", s_app_config.enable_spdif_sender);
    }
    
    char ip_str[16];
    size_t ip_len = sizeof(ip_str);
    err = nvs_get_str(nvs_handle, NVS_KEY_SENDER_DEST_IP, ip_str, &ip_len);
    if (err == ESP_OK) {
        strncpy(s_app_config.sender_destination_ip, ip_str, 15);
        s_app_config.sender_destination_ip[15] = '\0'; // Ensure null termination
    }
    
    err = nvs_get_u16(nvs_handle, NVS_KEY_SENDER_DEST_PORT, &u16_value);
    if (err == ESP_OK) {
        s_app_config.sender_destination_port = u16_value;
    }
    
    // Read audio processing settings
    err = nvs_get_u8(nvs_handle, NVS_KEY_USE_DIRECT_WRITE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.use_direct_write = (bool)u8_value;
    }
    
    // Read mDNS discovery settings
    err = nvs_get_u8(nvs_handle, NVS_KEY_ENABLE_MDNS_DISCOVERY, &u8_value);
    if (err == ESP_OK) {
        s_app_config.enable_mdns_discovery = (bool)u8_value;
    }
    
    err = nvs_get_u32(nvs_handle, NVS_KEY_DISCOVERY_INTERVAL_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.discovery_interval_ms = u32_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_AUTO_SELECT_DEVICE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.auto_select_best_device = (bool)u8_value;
    }
    
    // Read setup wizard status
    err = nvs_get_u8(nvs_handle, NVS_KEY_SETUP_WIZARD_COMPLETED, &u8_value);
    if (err == ESP_OK) {
        s_app_config.setup_wizard_completed = (bool)u8_value;
    }

    // Read SAP stream name
    size_t sap_len = sizeof(s_app_config.sap_stream_name);
    err = nvs_get_str(nvs_handle, NVS_KEY_SAP_STREAM_NAME, s_app_config.sap_stream_name, &sap_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading SAP stream name: %s", esp_err_to_name(err));
    }

    // Read device mode (new enum-based configuration)
    err = nvs_get_u8(nvs_handle, NVS_KEY_DEVICE_MODE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.device_mode = (device_mode_t)u8_value;
        ESP_LOGI(TAG, "Loaded device_mode: %d", s_app_config.device_mode);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // If device_mode is not found, derive it from legacy boolean fields
        if (s_app_config.enable_usb_sender) {
            s_app_config.device_mode = MODE_SENDER_USB;
            ESP_LOGI(TAG, "Derived device_mode from legacy: MODE_SENDER_USB");
        } else if (s_app_config.enable_spdif_sender) {
            s_app_config.device_mode = MODE_SENDER_SPDIF;
            ESP_LOGI(TAG, "Derived device_mode from legacy: MODE_SENDER_SPDIF");
        } else {
            // Default to receiver mode based on build type
            #ifdef IS_USB
            s_app_config.device_mode = MODE_RECEIVER_USB;
            ESP_LOGI(TAG, "Derived device_mode from legacy: MODE_RECEIVER_USB");
            #elif defined(IS_SPDIF)
            s_app_config.device_mode = MODE_RECEIVER_SPDIF;
            ESP_LOGI(TAG, "Derived device_mode from legacy: MODE_RECEIVER_SPDIF");
            #else
            s_app_config.device_mode = MODE_RECEIVER_USB;
            ESP_LOGI(TAG, "Derived device_mode from legacy: MODE_RECEIVER_USB (fallback)");
            #endif
        }
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Configuration loaded");
    return ESP_OK;
}

/**
 * Get the current configuration
 */
app_config_t* config_manager_get_config(void) {
    return &s_app_config;
}

/**
 * Reload configuration from NVS
 */
esp_err_t config_manager_reload(void) {
    ESP_LOGI(TAG, "Reloading configuration from NVS");
    
    // Store current values in case we need to fall back
    app_config_t backup_config = s_app_config;
    
    // Try to load all settings from NVS
    esp_err_t err = config_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reload configuration: %s", esp_err_to_name(err));
        // Restore previous values
        s_app_config = backup_config;
        return err;
    }
    
    ESP_LOGI(TAG, "Configuration reloaded successfully");
    return ESP_OK;
}

/**
 * Save configuration to NVS
 */
esp_err_t config_manager_save_config(void) {
    ESP_LOGI(TAG, "Saving configuration to NVS");
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Save network settings
    err = nvs_set_u16(nvs_handle, NVS_KEY_PORT, s_app_config.port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving port: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Save hostname
    err = nvs_set_str(nvs_handle, NVS_KEY_HOSTNAME, s_app_config.hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving hostname: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP SSID
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, s_app_config.ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving AP SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP password
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASSWORD, s_app_config.ap_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving AP password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save hide AP when connected setting
    err = nvs_set_u8(nvs_handle, NVS_KEY_HIDE_AP_CONNECTED, (uint8_t)s_app_config.hide_ap_when_connected);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving hide AP setting: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP-only mode setting
    err = nvs_set_u8(nvs_handle, NVS_KEY_AP_ONLY_MODE, (uint8_t)s_app_config.ap_only_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving AP-only mode setting: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save buffer settings
    err = nvs_set_u8(nvs_handle, NVS_KEY_INIT_BUF_SIZE, s_app_config.initial_buffer_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving initial buffer size: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_BUF_GROW_STEP, s_app_config.buffer_grow_step_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving buffer grow step: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_MAX_BUF_SIZE, s_app_config.max_buffer_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving max buffer size: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_MAX_GROW_SIZE, s_app_config.max_grow_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving max grow size: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save audio settings
    err = nvs_set_u32(nvs_handle, NVS_KEY_SAMPLE_RATE, s_app_config.sample_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving sample rate: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_BIT_DEPTH, s_app_config.bit_depth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving bit depth: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Store volume as integer (float * 100) for NVS
    uint32_t volume_int = (uint32_t)(s_app_config.volume * 100.0f);
    err = nvs_set_u32(nvs_handle, NVS_KEY_VOLUME, volume_int);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving volume: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save SPDIF data pin
    err = nvs_set_u8(nvs_handle, NVS_KEY_SPDIF_DATA_PIN, s_app_config.spdif_data_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving SPDIF data pin: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Saved SPDIF data pin: %d", s_app_config.spdif_data_pin);
    
    // Save sleep settings
    err = nvs_set_u32(nvs_handle, NVS_KEY_SILENCE_THRES_MS, s_app_config.silence_threshold_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving silence threshold: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u32(nvs_handle, NVS_KEY_NET_CHECK_MS, s_app_config.network_check_interval_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving network check interval: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_ACTIVITY_PACKETS, s_app_config.activity_threshold_packets);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving activity threshold packets: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u16(nvs_handle, NVS_KEY_SILENCE_AMPLT, s_app_config.silence_amplitude_threshold);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving silence amplitude threshold: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u32(nvs_handle, NVS_KEY_NET_INACT_MS, s_app_config.network_inactivity_timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving network inactivity timeout: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save USB Scream Sender settings
    err = nvs_set_u8(nvs_handle, NVS_KEY_ENABLE_USB_SENDER, (uint8_t)s_app_config.enable_usb_sender);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving USB sender enable: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save S/PDIF Scream Sender setting
    err = nvs_set_u8(nvs_handle, NVS_KEY_ENABLE_SPDIF_SENDER, (uint8_t)s_app_config.enable_spdif_sender);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving S/PDIF sender enable: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Saved enable_spdif_sender: %d", s_app_config.enable_spdif_sender);
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SENDER_DEST_IP, s_app_config.sender_destination_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving sender destination IP: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u16(nvs_handle, NVS_KEY_SENDER_DEST_PORT, s_app_config.sender_destination_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving sender destination port: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save direct write setting
    err = nvs_set_u8(nvs_handle, NVS_KEY_USE_DIRECT_WRITE, (uint8_t)s_app_config.use_direct_write);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving direct write setting: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Saved direct write setting: %d", s_app_config.use_direct_write);
    
    // Commit the changes (fully stop visualizer to avoid RMT during flash commit)
    bool viz_was_active = visualizer_is_active();
    if (viz_was_active) {
        visualizer_deinit();
    }
    err = nvs_commit(nvs_handle);
    if (viz_was_active) {
        visualizer_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Verify the save by reading back the value
    uint8_t saved_value;
    err = nvs_get_u8(nvs_handle, NVS_KEY_USE_DIRECT_WRITE, &saved_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error verifying direct write setting: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    if (saved_value != (uint8_t)s_app_config.use_direct_write) {
        ESP_LOGE(TAG, "Direct write setting verification failed: saved=%d, expected=%d", 
                 saved_value, (uint8_t)s_app_config.use_direct_write);
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Direct write setting verified: %d", saved_value);
    
    // Save mDNS discovery settings
    err = nvs_set_u8(nvs_handle, NVS_KEY_ENABLE_MDNS_DISCOVERY, (uint8_t)s_app_config.enable_mdns_discovery);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving mDNS discovery enable: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u32(nvs_handle, NVS_KEY_DISCOVERY_INTERVAL_MS, s_app_config.discovery_interval_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving discovery interval: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_AUTO_SELECT_DEVICE, (uint8_t)s_app_config.auto_select_best_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving auto select device: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save setup wizard status
    err = nvs_set_u8(nvs_handle, NVS_KEY_SETUP_WIZARD_COMPLETED, (uint8_t)s_app_config.setup_wizard_completed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving setup wizard status: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Save SAP stream name
    err = nvs_set_str(nvs_handle, NVS_KEY_SAP_STREAM_NAME, s_app_config.sap_stream_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving SAP stream name: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Save device mode (new enum-based configuration)
    err = nvs_set_u8(nvs_handle, NVS_KEY_DEVICE_MODE, (uint8_t)s_app_config.device_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving device mode: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Saved device_mode: %d", s_app_config.device_mode);
    
    // Commit the changes (fully stop visualizer to avoid RMT during flash commit)
    viz_was_active = visualizer_is_active();
    if (viz_was_active) {
        visualizer_deinit();
    }
    err = nvs_commit(nvs_handle);
    if (viz_was_active) {
        visualizer_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Configuration saved and verified successfully");
    return ESP_OK;
}

/**
 * Save a specific setting to NVS
 */
esp_err_t config_manager_save_setting(const char* key, void* value, size_t size) {
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving setting %s to NVS", key);
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Update in-memory configuration
    if (strcmp(key, NVS_KEY_PORT) == 0 && size == sizeof(uint16_t)) {
        s_app_config.port = *(uint16_t*)value;
        err = nvs_set_u16(nvs_handle, key, *(uint16_t*)value);
    } else if (strcmp(key, NVS_KEY_HOSTNAME) == 0) {
        strncpy(s_app_config.hostname, (char*)value, sizeof(s_app_config.hostname) - 1);
        s_app_config.hostname[sizeof(s_app_config.hostname) - 1] = '\0';
        err = nvs_set_str(nvs_handle, key, s_app_config.hostname);
    } else if (strcmp(key, NVS_KEY_AP_SSID) == 0) {
        strncpy(s_app_config.ap_ssid, (char*)value, WIFI_SSID_MAX_LENGTH);
        s_app_config.ap_ssid[WIFI_SSID_MAX_LENGTH] = '\0'; // Ensure null termination
        err = nvs_set_str(nvs_handle, key, s_app_config.ap_ssid);
    } else if (strcmp(key, NVS_KEY_AP_PASSWORD) == 0) {
        strncpy(s_app_config.ap_password, (char*)value, WIFI_PASSWORD_MAX_LENGTH);
        s_app_config.ap_password[WIFI_PASSWORD_MAX_LENGTH] = '\0'; // Ensure null termination
        err = nvs_set_str(nvs_handle, key, s_app_config.ap_password);
    } else if (strcmp(key, NVS_KEY_HIDE_AP_CONNECTED) == 0 && size == sizeof(bool)) {
        s_app_config.hide_ap_when_connected = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.hide_ap_when_connected);
    } else if (strcmp(key, NVS_KEY_AP_ONLY_MODE) == 0 && size == sizeof(bool)) {
        s_app_config.ap_only_mode = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.ap_only_mode);
    } else if (strcmp(key, NVS_KEY_INIT_BUF_SIZE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.initial_buffer_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_BUF_GROW_STEP) == 0 && size == sizeof(uint8_t)) {
        s_app_config.buffer_grow_step_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_MAX_BUF_SIZE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.max_buffer_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_MAX_GROW_SIZE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.max_grow_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_SAMPLE_RATE) == 0 && size == sizeof(uint32_t)) {
        s_app_config.sample_rate = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_BIT_DEPTH) == 0 && size == sizeof(uint8_t)) {
        s_app_config.bit_depth = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_VOLUME) == 0 && size == sizeof(float)) {
        s_app_config.volume = *(float*)value;
        uint32_t volume_int = (uint32_t)(s_app_config.volume * 100.0f);
        err = nvs_set_u32(nvs_handle, key, volume_int);
    } else if (strcmp(key, NVS_KEY_SPDIF_DATA_PIN) == 0 && size == sizeof(uint8_t)) {
        s_app_config.spdif_data_pin = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, s_app_config.spdif_data_pin);
        ESP_LOGI(TAG, "Saving SPDIF data pin value: %d", s_app_config.spdif_data_pin);
    } else if (strcmp(key, NVS_KEY_SILENCE_THRES_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.silence_threshold_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_NET_CHECK_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.network_check_interval_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_ACTIVITY_PACKETS) == 0 && size == sizeof(uint8_t)) {
        s_app_config.activity_threshold_packets = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_SILENCE_AMPLT) == 0 && size == sizeof(uint16_t)) {
        s_app_config.silence_amplitude_threshold = *(uint16_t*)value;
        err = nvs_set_u16(nvs_handle, key, *(uint16_t*)value);
    } else if (strcmp(key, NVS_KEY_NET_INACT_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.network_inactivity_timeout_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_ENABLE_USB_SENDER) == 0 && size == sizeof(bool)) {
        s_app_config.enable_usb_sender = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.enable_usb_sender);
    } else if (strcmp(key, NVS_KEY_ENABLE_SPDIF_SENDER) == 0 && size == sizeof(bool)) {
        s_app_config.enable_spdif_sender = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.enable_spdif_sender);
        ESP_LOGI(TAG, "Updating enable_spdif_sender to: %d", s_app_config.enable_spdif_sender);
    } else if (strcmp(key, NVS_KEY_SENDER_DEST_IP) == 0) {
        strncpy(s_app_config.sender_destination_ip, (char*)value, 15);
        s_app_config.sender_destination_ip[15] = '\0'; // Ensure null termination
        err = nvs_set_str(nvs_handle, key, s_app_config.sender_destination_ip);
    } else if (strcmp(key, NVS_KEY_SENDER_DEST_PORT) == 0 && size == sizeof(uint16_t)) {
        s_app_config.sender_destination_port = *(uint16_t*)value;
        err = nvs_set_u16(nvs_handle, key, s_app_config.sender_destination_port);
    } else if (strcmp(key, NVS_KEY_USE_DIRECT_WRITE) == 0 && size == sizeof(bool)) {
        s_app_config.use_direct_write = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.use_direct_write);
    } else if (strcmp(key, NVS_KEY_ENABLE_MDNS_DISCOVERY) == 0 && size == sizeof(bool)) {
        s_app_config.enable_mdns_discovery = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.enable_mdns_discovery);
    } else if (strcmp(key, NVS_KEY_DISCOVERY_INTERVAL_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.discovery_interval_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, s_app_config.discovery_interval_ms);
    } else if (strcmp(key, NVS_KEY_AUTO_SELECT_DEVICE) == 0 && size == sizeof(bool)) {
        s_app_config.auto_select_best_device = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.auto_select_best_device);
    } else if (strcmp(key, NVS_KEY_SETUP_WIZARD_COMPLETED) == 0 && size == sizeof(bool)) {
        s_app_config.setup_wizard_completed = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.setup_wizard_completed);
    } else if (strcmp(key, NVS_KEY_SAP_STREAM_NAME) == 0) {
        strncpy(s_app_config.sap_stream_name, (char*)value, sizeof(s_app_config.sap_stream_name) - 1);
        s_app_config.sap_stream_name[sizeof(s_app_config.sap_stream_name) - 1] = '\0';
        err = nvs_set_str(nvs_handle, key, s_app_config.sap_stream_name);
    } else if (strcmp(key, NVS_KEY_DEVICE_MODE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.device_mode = (device_mode_t)(*(uint8_t*)value);
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.device_mode);
        ESP_LOGI(TAG, "Updating device_mode to: %d", s_app_config.device_mode);
        
        // Update legacy boolean fields based on new device_mode
        switch (s_app_config.device_mode) {
            case MODE_RECEIVER_USB:
            case MODE_RECEIVER_SPDIF:
                s_app_config.enable_usb_sender = false;
                s_app_config.enable_spdif_sender = false;
                break;
            case MODE_SENDER_USB:
                s_app_config.enable_usb_sender = true;
                s_app_config.enable_spdif_sender = false;
                break;
            case MODE_SENDER_SPDIF:
                s_app_config.enable_usb_sender = false;
                s_app_config.enable_spdif_sender = true;
                break;
        }
        // Also save the legacy fields to NVS
        nvs_set_u8(nvs_handle, NVS_KEY_ENABLE_USB_SENDER, (uint8_t)s_app_config.enable_usb_sender);
        nvs_set_u8(nvs_handle, NVS_KEY_ENABLE_SPDIF_SENDER, (uint8_t)s_app_config.enable_spdif_sender);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving setting %s: %s", key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit the changes (fully stop visualizer to avoid RMT during flash commit)
    bool viz_was_active2 = visualizer_is_active();
    if (viz_was_active2) {
        visualizer_deinit();
    }
    err = nvs_commit(nvs_handle);
    if (viz_was_active2) {
        visualizer_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    return err;
}

/**
 * Reset configuration to defaults
 */
esp_err_t config_manager_reset(void) {
    ESP_LOGI(TAG, "Resetting configuration to defaults");
    
    // Reset in-memory configuration to defaults
    set_default_config();
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // Namespace doesn't exist, so already at defaults
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Erase all settings in this namespace
    err = nvs_erase_all(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error erasing NVS namespace: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit the changes (fully stop visualizer to avoid RMT during flash commit)
    bool viz_was_active3 = visualizer_is_active();
    if (viz_was_active3) {
        visualizer_deinit();
    }
    err = nvs_commit(nvs_handle);
    if (viz_was_active3) {
        visualizer_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Configuration reset to defaults");
    return ESP_OK;
}
