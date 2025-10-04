#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "wifi_manager";

// WiFi band definitions
#define WIFI_BAND_2_4GHZ 0
#define WIFI_BAND_5GHZ   1

// Channel ranges for different bands
// 2.4 GHz: channels 1-14
// 5 GHz: channels 32-173
#define WIFI_FIRST_2_4GHZ_CHANNEL 1
#define WIFI_LAST_2_4GHZ_CHANNEL  14
#define WIFI_FIRST_5GHZ_CHANNEL   32

// Minimum RSSI threshold for 5GHz to be considered "good enough"
// If 5GHz signal is weaker than this, we might prefer 2.4GHz
#define WIFI_5GHZ_MIN_RSSI -70

// NVS key for band preference
#define WIFI_NVS_KEY_BAND_PREFERENCE "band_pref"

// Default band preference (1 = prefer 5GHz, 0 = no preference)
#define DEFAULT_BAND_PREFERENCE 1

// NVS namespace and keys for WiFi credentials
#define WIFI_NVS_NAMESPACE "wifi_config"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"

// Event group to signal WiFi connection events
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Static netif pointers for both STA and AP modes
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// Current state of the WiFi manager
static wifi_manager_state_t s_wifi_manager_state = WIFI_MANAGER_STATE_NOT_INITIALIZED;

// Flag to indicate we're in scan mode - used to prevent connection attempts
static bool s_in_scan_mode = false;
// Flag to indicate initialization is complete - prevents race conditions in ESP-IDF 5.5
static bool s_initialization_complete = false;

// Reconnect backoff timer - schedules STA reconnects without blocking the event loop
static TimerHandle_t s_reconnect_timer = NULL;
// Global retry counter used for exponential backoff
static int s_retry_num = 0;
// Timer callback forward declaration
static void wifi_reconnect_timer_cb(TimerHandle_t xTimer);

// Callback for event notifications
static wifi_manager_event_cb_t s_event_callback = NULL;
static void* s_event_callback_user_data = NULL;

// AP configuration storage
static wifi_manager_ap_config_t s_ap_config = {
    .ssid = "ESP32-AP",
    .password = "",
    .hide_when_sta_connected = false,
    .channel = WIFI_AP_CHANNEL,
    .max_connections = WIFI_AP_MAX_CONNECTIONS
};

// Forward declarations for internal functions
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static esp_err_t start_ap_mode(void);

/**
 * Notify registered callback of WiFi events
 */
static void wifi_manager_notify_event(wifi_manager_event_type_t type, void* event_data) {
    if (s_event_callback) {
        wifi_manager_event_t event = {
            .type = type
        };
        
        // Copy event-specific data
        switch(type) {
            case WIFI_MANAGER_EVENT_STA_GOT_IP:
                if (event_data) {
                    ip_event_got_ip_t* got_ip = (ip_event_got_ip_t*)event_data;
                    event.data.got_ip.ip = got_ip->ip_info.ip.addr;
                    event.data.got_ip.netmask = got_ip->ip_info.netmask.addr;
                    event.data.got_ip.gateway = got_ip->ip_info.gw.addr;
                }
                break;
            case WIFI_MANAGER_EVENT_STA_DISCONNECTED:
                if (event_data) {
                    wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)event_data;
                    event.data.sta_disconnected.reason = disconn->reason;
                }
                break;
            case WIFI_MANAGER_EVENT_AP_STA_CONNECTED:
                if (event_data) {
                    wifi_event_ap_staconnected_t* conn = (wifi_event_ap_staconnected_t*)event_data;
                    memcpy(event.data.ap_sta_connected.mac, conn->mac, 6);
                }
                break;
            case WIFI_MANAGER_EVENT_AP_STA_DISCONNECTED:
                if (event_data) {
                    wifi_event_ap_stadisconnected_t* disconn = (wifi_event_ap_stadisconnected_t*)event_data;
                    memcpy(event.data.ap_sta_disconnected.mac, disconn->mac, 6);
                }
                break;
            default:
                break;
        }
        
        s_event_callback(&event, s_event_callback_user_data);
    }
}

/**
 * Initialize the WiFi manager
 */
esp_err_t wifi_manager_init(void) {
    ESP_LOGI(TAG, "Initializing WiFi manager");
    
    // Suppress WiFi driver warning logs (like "exceed max band")
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    
    // Check if already initialized
    if (s_wifi_manager_state != WIFI_MANAGER_STATE_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }
    
    // Initialize NVS for WiFi credential storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Create the event group for WiFi events
    s_wifi_event_group = xEventGroupCreate();

    // Create reconnect timer used to schedule STA reconnect attempts
    s_reconnect_timer = xTimerCreate("wifi_reconnect", pdMS_TO_TICKS(1000), pdFALSE, NULL, wifi_reconnect_timer_cb);
    if (s_reconnect_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi reconnect timer");
        // Continue without timer; reconnect attempts will be skipped if timer is unavailable
    }
    
    // Initialize the TCP/IP stack (safely - it might be initialized already)
    esp_err_t net_err = esp_netif_init();
    if (net_err != ESP_OK && net_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(net_err);
    }
    
    // Create the default event loop, but don't error if it already exists
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    
    // Create default netif instances for both STA and AP - check if they already exist
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers for WiFi events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler,
                                                       NULL,
                                                       NULL));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler,
                                                       NULL,
                                                       NULL));
    
    // Set WiFi mode to APSTA (both AP and STA can be active)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTING;
    
    return ESP_OK;
}

/**
 * Event handler for WiFi events
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    
    if (event_base == WIFI_EVENT) {
        // Network events are handled by lifecycle_manager
        // No need to manually forward events
        
        if (event_id == WIFI_EVENT_STA_START) {
            // WiFi station started, attempt to connect, but only if not in scan mode and initialization is complete
            if (!s_in_scan_mode && s_initialization_complete) {
                ESP_LOGI(TAG, "STA started, connecting to AP");
                esp_wifi_connect();
            } else {
                ESP_LOGD(TAG, "STA started, but not connecting yet (scan_mode=%d, init_complete=%d, state=%d)",
                         s_in_scan_mode, s_initialization_complete, s_wifi_manager_state);
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disconn = event_data;
            wifi_manager_notify_event(WIFI_MANAGER_EVENT_STA_DISCONNECTED, disconn);
            
            // Re-enable AP mode if it was hidden while connected
            if (s_ap_config.hide_when_sta_connected) {
                ESP_LOGI(TAG, "Re-enabling AP interface after disconnection");
                esp_wifi_set_mode(WIFI_MODE_APSTA);
            }
            
            // Increment retry count and compute backoff delay (exponential, capped)
            s_retry_num++;
            int delay_ms = 1000; // Base delay of 1 second
            if (s_retry_num > 1) {
                // Exponential backoff with a maximum of 30 seconds
                delay_ms = (1 << (s_retry_num > 5 ? 5 : s_retry_num)) * 1000;
                if (delay_ms > 30000) {
                    delay_ms = 30000;
                }
            }

            // Signal failure so any waiters can proceed
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

            ESP_LOGI(TAG, "Connection attempt %d failed, reason: %" PRIu16 ", scheduling reconnect in %d ms",
                    s_retry_num, disconn->reason, delay_ms);

            // Schedule reconnect via timer to avoid blocking the event loop
            if (s_reconnect_timer) {
                xTimerStop(s_reconnect_timer, 0);
                xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
                xTimerStart(s_reconnect_timer, 0);
            }
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "Station connected to AP, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                   event->mac[0], event->mac[1], event->mac[2],
                   event->mac[3], event->mac[4], event->mac[5]);
            wifi_manager_notify_event(WIFI_MANAGER_EVENT_AP_STA_CONNECTED, event);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "Station disconnected from AP, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                   event->mac[0], event->mac[1], event->mac[2],
                   event->mac[3], event->mac[4], event->mac[5]);
            wifi_manager_notify_event(WIFI_MANAGER_EVENT_AP_STA_DISCONNECTED, event);
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP address: " IPSTR,
                    IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTED;
            wifi_manager_notify_event(WIFI_MANAGER_EVENT_STA_GOT_IP, event);

            // mDNS restart is handled by lifecycle_manager when network changes

            // If configured to hide AP when connected, disable AP interface
            if (s_ap_config.hide_when_sta_connected) {
                ESP_LOGI(TAG, "Disabling AP interface when connected (as configured)");
                esp_wifi_set_mode(WIFI_MODE_STA);
            }
        }
    }
}

// One-shot reconnect timer callback: attempts a safe STA reconnect without blocking the event loop
static void wifi_reconnect_timer_cb(TimerHandle_t xTimer) {
    // Only attempt reconnect when appropriate
    if (s_in_scan_mode || !s_initialization_complete) {
        ESP_LOGD(TAG, "Reconnect timer fired but conditions not met (scan_mode=%d, init_complete=%d)",
                 s_in_scan_mode, s_initialization_complete);
        return;
    }
    if (!wifi_manager_has_credentials()) {
        ESP_LOGW(TAG, "Reconnect skipped: no stored credentials");
        return;
    }
    ESP_LOGI(TAG, "Attempting STA reconnect");
    esp_wifi_connect();
}

/**
 * Start the WiFi manager
 */
esp_err_t wifi_manager_start(void) {
    ESP_LOGI(TAG, "Starting WiFi manager");
    
    // Initialize if not already initialized
    if (s_wifi_manager_state == WIFI_MANAGER_STATE_NOT_INITIALIZED) {
        ESP_ERROR_CHECK(wifi_manager_init());
    }
    
    // Get AP configuration from stored config
    const char* ap_ssid = s_ap_config.ssid;
    const char* ap_password = s_ap_config.password;
    
    // Configure AP mode
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Use custom AP SSID from config
    strncpy((char*)wifi_ap_config.ap.ssid, ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(ap_ssid);
    
    // Copy password if provided
    if (strlen(ap_password) > 0) {
        strncpy((char*)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    // NOTE: Removed duplicate AP config here - it's configured below with proper state management
    
    // Check if we have stored credentials
    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Found stored WiFi credentials, trying to connect");
        
        // Read stored credentials
        char ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
        char password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};
        
        nvs_handle_t nvs_handle;
        ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle));
        
        size_t required_size = sizeof(ssid);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid, &required_size));
        
        required_size = sizeof(password);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password, &required_size));
        
        nvs_close(nvs_handle);
        
        // Configure WiFi station with the stored credentials
        wifi_config_t wifi_sta_config = {0};
        
        strncpy((char*)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
        strncpy((char*)wifi_sta_config.sta.password, password, sizeof(wifi_sta_config.sta.password));
        
        // ESP-IDF 5.5 fix: Stop WiFi before reconfiguring to avoid state conflicts
        esp_err_t stop_ret = esp_wifi_stop();
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "WiFi stop returned: %s", esp_err_to_name(stop_ret));
        }
        
        // Set mode first before configuring interfaces
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        
        // Configure both interfaces
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
        
        // Start WiFi
        ESP_ERROR_CHECK(esp_wifi_start());
        
        // Mark initialization as complete and manually trigger connection
        s_initialization_complete = true;
        ESP_LOGI(TAG, "WiFi started, manually triggering connection");
        esp_wifi_connect();

        // Clear status bits before waiting
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        
        // Wait for connection with timeout
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECTION_TIMEOUT_MS));
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGI(TAG, "Failed to connect to SSID: %s", ssid);
            ESP_ERROR_CHECK(esp_wifi_stop());
            
            // Start AP mode since connection failed
            return start_ap_mode();
        } else {
            ESP_LOGE(TAG, "Connection timeout");
            ESP_ERROR_CHECK(esp_wifi_stop());
            
            // Start AP mode since connection timed out
            return start_ap_mode();
        }
    } else {
        ESP_LOGI(TAG, "No stored WiFi credentials, starting AP mode");
        
        // No credentials, start AP mode directly
        return start_ap_mode();
    }
}

/**
 * Start AP mode with captive portal
 */
static esp_err_t start_ap_mode(void) {
    ESP_LOGI(TAG, "Starting AP mode");
    
    // mDNS is started by lifecycle services; nothing to do here
     
    // Get AP configuration from stored config
    const char* ap_ssid = s_ap_config.ssid;
    const char* ap_password = s_ap_config.password;
    
    // Configure AP
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Use custom AP SSID from config
    strncpy((char*)wifi_ap_config.ap.ssid, ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(ap_ssid);
    
    // Copy password if provided
    if (strlen(ap_password) > 0) {
        strncpy((char*)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
        ESP_LOGI(TAG, "Using configured AP password (password protected)");
    } else {
        ESP_LOGI(TAG, "Using open AP (no password)");
    }
    
    // ESP-IDF 5.5 fix: Stop WiFi before reconfiguring
    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "WiFi stop returned: %s", esp_err_to_name(stop_ret));
    }
    
    // Configure DHCP server to advertise itself (192.168.4.1) as the DNS server for captive portal
    if (s_ap_netif) {
        esp_netif_dns_info_t dns_info;
        dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        
        esp_err_t ret = esp_netif_dhcps_stop(s_ap_netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "Failed to stop DHCP server: %s", esp_err_to_name(ret));
        }
        
        ret = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set DNS server info: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Configured DHCP to advertise 192.168.4.1 as DNS server for captive portal");
        }
        
        ret = esp_netif_dhcps_start(s_ap_netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "Failed to start DHCP server: %s", esp_err_to_name(ret));
        }
    }
    
    // Use APSTA mode (both interfaces were created in init)
    // The STA won't connect because we set the state to AP_MODE
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Mark initialization as complete after starting AP mode
    s_initialization_complete = true;
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_AP_MODE;
    
    ESP_LOGI(TAG, "AP started with SSID: %s", ap_ssid);
    
    // Web server will be started by the web_server module
    
    return ESP_OK;
}

/**
 * Get the current state of the WiFi manager
 */
wifi_manager_state_t wifi_manager_get_state(void) {
    return s_wifi_manager_state;
}

/**
 * Get the current connected SSID (if any)
 */
esp_err_t wifi_manager_get_current_ssid(char *ssid, size_t max_length) {
    if (s_wifi_manager_state != WIFI_MANAGER_STATE_CONNECTED) {
        strncpy(ssid, "Not configured", max_length);
        ssid[max_length - 1] = '\0';
        return ESP_FAIL;
    }
    
    wifi_config_t wifi_config;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
    
    strncpy(ssid, (char*)wifi_config.sta.ssid, max_length);
    ssid[max_length - 1] = '\0'; // Ensure null-termination
    
    return ESP_OK;
}

/**
 * Check if credentials are stored in NVS
 */
bool wifi_manager_has_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "WiFi credentials not found in NVS (namespace not found)");
        } else {
            ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        }
        return false;
    }
    
    size_t required_size = 0;
    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, NULL, &required_size);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "WiFi credentials not found in NVS (no SSID key)");
        } else {
            ESP_LOGE(TAG, "Error reading SSID from NVS: %s", esp_err_to_name(ret));
        }
        return false;
    }
    
    return true;
}

/**
 * Save WiFi credentials to NVS
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving WiFi credentials for SSID: %s", ssid);
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving SSID to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Save password (even if empty, which is valid for open networks)
    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password ? password : "");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving password to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    return ret;
}

esp_err_t wifi_manager_get_credentials(char *ssid, size_t ssid_max_len, char *password, size_t password_max_len) {
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid, &ssid_max_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password, &password_max_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * Clear stored WiFi credentials from NVS
 */
esp_err_t wifi_manager_clear_credentials(void) {
    ESP_LOGI(TAG, "Clearing stored WiFi credentials");
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            // Namespace doesn't exist, which means no credentials are stored
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_erase_all(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error erasing NVS namespace: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    return ret;
}

/**
 * Connect to a specific WiFi network
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    
    // Save the credentials to NVS first
    esp_err_t ret = wifi_manager_save_credentials(ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials");
        return ret;
    }
    
    // Stop current WiFi mode (ESP-IDF 5.5: must stop before reconfiguring)
    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "WiFi stop returned: %s", esp_err_to_name(stop_ret));
    }
    
    // Clear the status bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Configure WiFi station with the new credentials
    wifi_config_t wifi_sta_config = {0};
    
    strncpy((char*)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
    if (password) {
        strncpy((char*)wifi_sta_config.sta.password, password, sizeof(wifi_sta_config.sta.password));
    }
    
    // Get AP configuration from stored config
    const char* ap_ssid = s_ap_config.ssid;
    const char* ap_password = s_ap_config.password;
    
    // Configure AP mode
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Use custom AP SSID from config
    strncpy((char*)wifi_ap_config.ap.ssid, ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(ap_ssid);
    
    // Copy password if provided
    if (strlen(ap_password) > 0) {
        strncpy((char*)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    
    // ESP-IDF 5.5 fix: Set mode first, then configure interfaces
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    // Configure both interfaces after setting mode
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Ensure initialization flag is set
    s_initialization_complete = true;
    
    // Manually trigger connection after start
    ESP_LOGI(TAG, "WiFi reconfigured, triggering connection");
    esp_wifi_connect();
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTING;
    
    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                        pdFALSE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(WIFI_CONNECTION_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);
        s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTED;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", ssid);
        // Start AP mode again
        start_ap_mode();
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        // Start AP mode again
        start_ap_mode();
        return ESP_ERR_TIMEOUT;
    }
}

// Original function replaced with enhanced version below

/**
 * Stop the WiFi manager and release resources
 */
esp_err_t wifi_manager_stop(void) {
    ESP_LOGI(TAG, "Stopping WiFi manager");
    
    // Reset initialization flag when stopping
    s_initialization_complete = false;
    
    // Don't stop mDNS - it should remain running across WiFi restarts
    // mDNS can work in AP mode, STA mode, or APSTA mode
    
    // Stop WiFi
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Stop pending reconnect timer if any
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    
    // Don't reset to NOT_INITIALIZED - we're just stopping WiFi, not de-initializing everything
    // This prevents wifi_manager_start() from calling wifi_manager_init() again
    s_wifi_manager_state = WIFI_MANAGER_STATE_AP_MODE;  // Safe default state
    
    return ESP_OK;
}

/**
 * Determine WiFi band from channel number
 */
static uint8_t wifi_manager_get_band_from_channel(uint8_t channel) {
    if (channel >= WIFI_FIRST_2_4GHZ_CHANNEL && channel <= WIFI_LAST_2_4GHZ_CHANNEL) {
        return WIFI_BAND_2_4GHZ;
    } else if (channel >= WIFI_FIRST_5GHZ_CHANNEL) {
        return WIFI_BAND_5GHZ;
    } else {
        // Default to 2.4GHz for unknown channels
        return WIFI_BAND_2_4GHZ;
    }
}

/**
 * Get the band preference setting
 */
static uint8_t wifi_manager_get_band_preference(void) {
    nvs_handle_t nvs_handle;
    uint8_t band_preference = DEFAULT_BAND_PREFERENCE;
    
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        uint8_t value;
        ret = nvs_get_u8(nvs_handle, WIFI_NVS_KEY_BAND_PREFERENCE, &value);
        if (ret == ESP_OK) {
            band_preference = value;
        }
        nvs_close(nvs_handle);
    }
    
    return band_preference;
}

/**
 * Set the band preference setting
 */
esp_err_t wifi_manager_set_band_preference(uint8_t preference) {
    ESP_LOGI(TAG, "Setting band preference to %d", preference);
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_u8(nvs_handle, WIFI_NVS_KEY_BAND_PREFERENCE, preference);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving band preference to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    return ret;
}

/**
 * Scan for available WiFi networks
 */
esp_err_t wifi_manager_scan_networks(wifi_network_info_t *networks, size_t max_networks,
                                    size_t *networks_found) {
    ESP_LOGI(TAG, "Scanning for WiFi networks");
    
    if (!networks || max_networks == 0 || !networks_found) {
        ESP_LOGE(TAG, "Invalid arguments for network scan");
        return ESP_ERR_INVALID_ARG;
    }
    
    *networks_found = 0;

    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // Block connection attempts during scan
    s_in_scan_mode = true;

    bool mode_changed = false;
    wifi_ap_record_t *ap_records = NULL;
    uint16_t num_ap = 0;

    // If AP-only, switch to APSTA temporarily
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Temporarily switching to APSTA mode for scanning");
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to APSTA mode: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        mode_changed = true;
    }

    // Scan configuration
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 0
    };

    // Start scan
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi scan: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Get number of APs
    ret = esp_wifi_scan_get_ap_num(&num_ap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP scan count: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    if (num_ap == 0) {
        ESP_LOGI(TAG, "No networks found");
        *networks_found = 0;
        ret = ESP_OK;
        goto cleanup;
    }

    if (num_ap > max_networks) {
        num_ap = max_networks;
    }

    ap_records = malloc(sizeof(wifi_ap_record_t) * num_ap);
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ret = esp_wifi_scan_get_ap_records(&num_ap, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP scan records: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Copy results
    for (int i = 0; i < num_ap; i++) {
        strncpy(networks[i].ssid, (char *)ap_records[i].ssid, WIFI_SSID_MAX_LENGTH);
        networks[i].ssid[WIFI_SSID_MAX_LENGTH] = '\0'; // Ensure null-termination
        networks[i].rssi = ap_records[i].rssi;
        networks[i].authmode = ap_records[i].authmode;
        networks[i].channel = ap_records[i].primary;
        networks[i].band = wifi_manager_get_band_from_channel(ap_records[i].primary);

        ESP_LOGD(TAG, "Network: %s, Channel: %d, Band: %s, RSSI: %d",
                 networks[i].ssid, networks[i].channel,
                 networks[i].band == WIFI_BAND_5GHZ ? "5GHz" : "2.4GHz",
                 networks[i].rssi);
    }

    *networks_found = num_ap;
    ret = ESP_OK;

cleanup:
    if (ap_records) {
        free(ap_records);
    }
    if (mode_changed) {
        ESP_LOGI(TAG, "Restoring original WiFi mode after scan");
        esp_wifi_set_mode(current_mode);
    }
    // Always clear scan flag before returning
    s_in_scan_mode = false;
    return ret;
}

/**
 * Connect to strongest available network with band preference
 */
esp_err_t wifi_manager_connect_to_strongest(void) {
    ESP_LOGI(TAG, "Scanning and connecting to strongest network with band preference");
    
    // ESP-IDF 5.5 fix: Stop WiFi completely before reconfiguring
    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "WiFi stop returned: %s", esp_err_to_name(stop_ret));
    }
    
    // Set APSTA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Set flags BEFORE starting WiFi to prevent premature connection attempts
    s_initialization_complete = true;
    s_in_scan_mode = true;  // Block event handler from connecting until we're ready

    // Start WiFi
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        return err;
    }
    
    // Clear the status bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Scan for networks - use maximum scan results (30 is a reasonable limit)
    #define MAX_SCAN_RESULTS 30
    wifi_network_info_t networks[MAX_SCAN_RESULTS];
    size_t networks_found = 0;
    
    esp_err_t ret = wifi_manager_scan_networks(networks, MAX_SCAN_RESULTS, &networks_found);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to scan networks: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (networks_found == 0) {
        ESP_LOGI(TAG, "No networks found");
        start_ap_mode();
        return ESP_FAIL;
    }
    
    // Get stored SSID to only connect to previously configured networks
    nvs_handle_t nvs_handle;
    char stored_ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char stored_password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};
    bool has_stored_credentials = false;
    
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t required_size = sizeof(stored_ssid);
        if (nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, stored_ssid, &required_size) == ESP_OK) {
            // Also get the password
            required_size = sizeof(stored_password);
            nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, stored_password, &required_size);
            has_stored_credentials = true;
        }
        nvs_close(nvs_handle);
    }
    
    if (!has_stored_credentials) {
        ESP_LOGI(TAG, "No stored WiFi credentials found, cannot connect to any network");
        return ESP_FAIL;
    }
    
    // Get band preference
    uint8_t band_preference = wifi_manager_get_band_preference();
    ESP_LOGI(TAG, "Band preference: %s", band_preference ? "5GHz preferred" : "No preference");
    
    // Find matching networks for our stored SSID
    int best_5ghz_index = -1;
    int best_5ghz_rssi = -128;
    int best_2_4ghz_index = -1;
    int best_2_4ghz_rssi = -128;
    
    for (int i = 0; i < networks_found; i++) {
        // Only consider networks that match our stored SSID
        if (strcmp(networks[i].ssid, stored_ssid) == 0) {
            if (networks[i].band == WIFI_BAND_5GHZ) {
                // 5GHz network
                if (networks[i].rssi > best_5ghz_rssi) {
                    best_5ghz_rssi = networks[i].rssi;
                    best_5ghz_index = i;
                }
            } else {
                // 2.4GHz network
                if (networks[i].rssi > best_2_4ghz_rssi) {
                    best_2_4ghz_rssi = networks[i].rssi;
                    best_2_4ghz_index = i;
                }
            }
        }
    }
    
    // Determine which network to connect to based on band preference
    int selected_index = -1;
    
    if (band_preference && best_5ghz_index != -1) {
        // We prefer 5GHz and found a 5GHz network
        if (best_5ghz_rssi >= WIFI_5GHZ_MIN_RSSI) {
            // 5GHz signal is strong enough
            selected_index = best_5ghz_index;
            ESP_LOGI(TAG, "Selected 5GHz network with RSSI: %d", best_5ghz_rssi);
        } else if (best_2_4ghz_index != -1) {
            // 5GHz signal is weak, fall back to 2.4GHz if available
            selected_index = best_2_4ghz_index;
            ESP_LOGI(TAG, "5GHz signal too weak (RSSI: %d), falling back to 2.4GHz with RSSI: %d", 
                    best_5ghz_rssi, best_2_4ghz_rssi);
        } else {
            // No 2.4GHz fallback, use weak 5GHz anyway
            selected_index = best_5ghz_index;
            ESP_LOGI(TAG, "Using weak 5GHz network (RSSI: %d) as no 2.4GHz alternative found", 
                    best_5ghz_rssi);
        }
    } else if (best_5ghz_index != -1 && best_2_4ghz_index != -1) {
        // No specific preference, choose the stronger signal
        if (best_5ghz_rssi > best_2_4ghz_rssi) {
            selected_index = best_5ghz_index;
            ESP_LOGI(TAG, "Selected 5GHz network with stronger signal (RSSI: %d vs %d)", 
                    best_5ghz_rssi, best_2_4ghz_rssi);
        } else {
            selected_index = best_2_4ghz_index;
            ESP_LOGI(TAG, "Selected 2.4GHz network with stronger signal (RSSI: %d vs %d)", 
                    best_2_4ghz_rssi, best_5ghz_rssi);
        }
    } else if (best_5ghz_index != -1) {
        // Only found 5GHz
        selected_index = best_5ghz_index;
        ESP_LOGI(TAG, "Selected only available 5GHz network (RSSI: %d)", best_5ghz_rssi);
    } else if (best_2_4ghz_index != -1) {
        // Only found 2.4GHz
        selected_index = best_2_4ghz_index;
        ESP_LOGI(TAG, "Selected only available 2.4GHz network (RSSI: %d)", best_2_4ghz_rssi);
    } else {
        ESP_LOGI(TAG, "No matching networks found for SSID: %s", stored_ssid);
        return ESP_FAIL;
    }
    
    if (selected_index == -1) {
        ESP_LOGI(TAG, "Failed to select a network");
        return ESP_FAIL;
    }
    
    // Connect to the selected network
    ESP_LOGI(TAG, "Connecting to network: %s (Channel: %d, Band: %s, RSSI: %d)", 
             networks[selected_index].ssid, 
             networks[selected_index].channel,
             networks[selected_index].band == WIFI_BAND_5GHZ ? "5GHz" : "2.4GHz",
             networks[selected_index].rssi);
    
    // Configure WiFi station with the selected network
    wifi_config_t wifi_sta_config = {0};
    
    strncpy((char*)wifi_sta_config.sta.ssid, networks[selected_index].ssid, sizeof(wifi_sta_config.sta.ssid));
    strncpy((char*)wifi_sta_config.sta.password, stored_password, sizeof(wifi_sta_config.sta.password));
    
    // Update WiFi configuration (can be done while WiFi is running)
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTING;

    // Prevent event handler from calling esp_wifi_connect() while we do it manually
    s_initialization_complete = false;
    s_in_scan_mode = false;

    // Disconnect first if already connected, then connect to new network
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Re-enable event handler for handling disconnections/reconnections
    s_initialization_complete = true;
    
    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                        pdFALSE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(WIFI_CONNECTION_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to network: %s", networks[selected_index].ssid);
        s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTED;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to network: %s", networks[selected_index].ssid);
        start_ap_mode();
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        start_ap_mode();
        return ESP_ERR_TIMEOUT;
    }
}
/**
 * Initialize WiFi manager with custom AP configuration
 */
esp_err_t wifi_manager_init_with_config(const wifi_manager_ap_config_t* ap_config) {
    if (ap_config) {
        memcpy(&s_ap_config, ap_config, sizeof(wifi_manager_ap_config_t));
    }
    return wifi_manager_init();
}

/**
 * Register a callback for WiFi events
 */
esp_err_t wifi_manager_register_event_callback(wifi_manager_event_cb_t cb, void* user_data) {
    s_event_callback = cb;
    s_event_callback_user_data = user_data;
    return ESP_OK;
}

/**
 * Unregister the WiFi event callback
 */
esp_err_t wifi_manager_unregister_event_callback(void) {
    s_event_callback = NULL;
    s_event_callback_user_data = NULL;
    return ESP_OK;
}

/**
 * Update AP configuration
 */
esp_err_t wifi_manager_set_ap_config(const wifi_manager_ap_config_t* ap_config) {
    if (!ap_config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&s_ap_config, ap_config, sizeof(wifi_manager_ap_config_t));
    
    // If WiFi is running and in AP mode, apply the new configuration
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
            // Reconfigure AP
            wifi_config_t wifi_ap_config = {
                .ap = {
                    .ssid_len = strlen(s_ap_config.ssid),
                    .channel = s_ap_config.channel,
                    .max_connection = s_ap_config.max_connections,
                    .authmode = strlen(s_ap_config.password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
                },
            };
            
            strncpy((char*)wifi_ap_config.ap.ssid, s_ap_config.ssid, sizeof(wifi_ap_config.ap.ssid));
            if (strlen(s_ap_config.password) > 0) {
                strncpy((char*)wifi_ap_config.ap.password, s_ap_config.password, sizeof(wifi_ap_config.ap.password));
            }
            
            return esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
        }
    }
    
    return ESP_OK;
}

