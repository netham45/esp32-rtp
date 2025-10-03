#include "mdns_service.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "mdns.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lifecycle_manager.h"
#include "bq25895_integration.h"

static const char *TAG = "mdns_service";

// mDNS constants
#define MDNS_SERVICE_NAME "scream"      // No underscore - mdns_service_add adds it
#define MDNS_SERVICE_PROTO "udp"        // No underscore - mdns_service_add adds it

// Global instance name with MAC suffix
static char g_instance_name[32] = {0};
// Global hostname with MAC suffix
static char g_hostname[32] = {0};

// Service state
static bool s_mdns_initialized = false;
static bool s_advertisement_enabled = false;

// Static buffers for TXT record strings that need to persist
static char s_battery_level_str[8];
static char s_mac_str[18];  // xx:xx:xx:xx:xx:xx + null terminator

// Task for periodic TXT record updates
static TaskHandle_t s_txt_update_task = NULL;
static volatile bool s_txt_task_running = false;

// Helper function to calculate battery percentage from voltage
// Assuming Li-Ion battery: 4.2V = 100%, 3.0V = 0%
static int calculate_battery_percentage(float voltage) {
    if (voltage >= 4.2f) return 100;
    if (voltage <= 3.0f) return 0;
    // Linear interpolation between 3.0V and 4.2V
    return (int)((voltage - 3.0f) / 1.2f * 100.0f);
}

// Helper function to get battery level string
static void get_battery_level_string(char *buf, size_t buf_size) {
    bq25895_status_t status;
    if (bq25895_integration_get_status(&status) == ESP_OK) {
        int percentage = calculate_battery_percentage(status.bat_voltage);
        snprintf(buf, buf_size, "%d", percentage);
    } else {
        // Default to 100 if we can't read battery status
        snprintf(buf, buf_size, "100");
    }
}

/**
 * @brief Generate instance name and hostname using configured hostname
 */
static void generate_instance_name(void) {
    // Use the configured hostname from lifecycle manager
    const char *configured_hostname = lifecycle_get_hostname();

    if (configured_hostname && strlen(configured_hostname) > 0) {
        // Use the configured hostname
        strncpy(g_hostname, configured_hostname, sizeof(g_hostname) - 1);
        g_hostname[sizeof(g_hostname) - 1] = '\0';

        // Use hostname as instance name too
        strncpy(g_instance_name, configured_hostname, sizeof(g_instance_name) - 1);
        g_instance_name[sizeof(g_instance_name) - 1] = '\0';

        ESP_LOGD(TAG, "Using configured hostname: %s", g_hostname);
        ESP_LOGD(TAG, "Using configured instance name: %s", g_instance_name);
        return;
    }

    // Fallback: generate with MAC address if no hostname configured
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            // Last resort fallback
            ESP_LOGW(TAG, "Failed to get MAC address, using default names");
            bool enable_usb_sender = lifecycle_get_enable_usb_sender();
            bool enable_spdif_sender = lifecycle_get_enable_spdif_sender();
            const char *device_type = (enable_usb_sender || enable_spdif_sender) ?
                                     "rtp-sender" : "rtp-receiver";
            snprintf(g_instance_name, sizeof(g_instance_name), "%s", device_type);
            snprintf(g_hostname, sizeof(g_hostname), "esp32-scream");
            return;
        }
    }

    // Generate fallback names with MAC
    bool enable_usb_sender = lifecycle_get_enable_usb_sender();
    bool enable_spdif_sender = lifecycle_get_enable_spdif_sender();
    const char *device_type = (enable_usb_sender || enable_spdif_sender) ?
                             "rtp-sender" : "rtp-receiver";

    snprintf(g_instance_name, sizeof(g_instance_name), "%s%02X%02X%02X",
             device_type, mac[3], mac[4], mac[5]);
    snprintf(g_hostname, sizeof(g_hostname), "esp32-scream-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGD(TAG, "Generated fallback instance name: %s", g_instance_name);
    ESP_LOGD(TAG, "Generated fallback hostname: %s", g_hostname);
}

/**
 * @brief Update TXT records for the mDNS service
 */
static esp_err_t update_txt_records(void) {
    if (!s_advertisement_enabled) {
        return ESP_OK;
    }

    // Determine mode and type
    bool enable_usb_sender = lifecycle_get_enable_usb_sender();
    bool enable_spdif_sender = lifecycle_get_enable_spdif_sender();
    const char *mode = (enable_usb_sender || enable_spdif_sender) ? "sender" : "receiver";
    const char *type = "unknown";
    if (enable_spdif_sender) {
        type = "spdif";
    } else if (enable_usb_sender) {
        type = "usb";
    } else {
        type = "receiver";
    }

    // Get current battery level (use static buffer)
    get_battery_level_string(s_battery_level_str, sizeof(s_battery_level_str));

    // Get MAC address for TXT record (use static buffer)
    uint8_t mac[6];
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (mac_err != ESP_OK) {
        mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (mac_err == ESP_OK) {
        snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strncpy(s_mac_str, "unknown", sizeof(s_mac_str));
    }

    // Hard-coded values that don't change
    const char *samplerates = "44100,48000";
    const char *codecs = "lpcm";
    const char *channels = "2";

    // Prepare updated TXT records
    mdns_txt_item_t txt_records[] = {
        {"mode", mode},
        {"type", type},
        {"battery", s_battery_level_str},
        {"mac", s_mac_str},
        {"samplerates", samplerates},
        {"codecs", codecs},
        {"channels", channels}
    };

    // Update the TXT records for the service
    esp_err_t err = mdns_service_txt_set("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO,
                                          txt_records, sizeof(txt_records)/sizeof(txt_records[0]));

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "✓ TXT records updated: battery=%s%%, mode=%s, type=%s",
                 s_battery_level_str, mode, type);
    } else {
        ESP_LOGW(TAG, "Failed to update TXT records: %s", esp_err_to_name(err));
    }

    return err;
}

/**
 * @brief Task for periodic TXT record updates
 */
static void txt_update_task(void *pvParameters) {
    ESP_LOGI(TAG, "TXT record update task started");
    const int UPDATE_INTERVAL = 30; // Update every 30 seconds

    while (s_txt_task_running) {
        vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL * 1000));

        if (s_txt_task_running && s_advertisement_enabled) {
            ESP_LOGD(TAG, "Periodic TXT record update");
            update_txt_records();
        }
    }

    ESP_LOGI(TAG, "TXT record update task stopped");
    s_txt_update_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Initialize mDNS service (call once during system init)
 */
esp_err_t mdns_service_init(void) {
    ESP_LOGI(TAG, "Initializing mDNS service");

    if (s_mdns_initialized) {
        ESP_LOGW(TAG, "mDNS already initialized");
        return ESP_OK;
    }

    // Generate instance name with MAC suffix if not already done
    if (g_instance_name[0] == '\0') {
        generate_instance_name();
    }

    // Initialize esp_mdns
    ESP_LOGI(TAG, "Calling mdns_init()");
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init() failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set hostname
    ESP_LOGI(TAG, "Setting hostname to '%s'", g_hostname);
    err = mdns_hostname_set(g_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
    }

    // Set instance name
    ESP_LOGI(TAG, "Setting instance name to '%s'", g_instance_name);
    err = mdns_instance_name_set(g_instance_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set instance name: %s", esp_err_to_name(err));
    }

    s_mdns_initialized = true;
    ESP_LOGI(TAG, "mDNS service initialized successfully");

    return ESP_OK;
}

/**
 * @brief Start mDNS service advertisement
 */
void mdns_service_start(void) {
    ESP_LOGI(TAG, "Starting mDNS service advertisement");

    if (!s_mdns_initialized) {
        ESP_LOGE(TAG, "mDNS not initialized! Call mdns_service_init() first");
        return;
    }

    if (s_advertisement_enabled) {
        ESP_LOGW(TAG, "Advertisement already enabled");
        return;
    }

    // Always regenerate instance name to pick up any hostname changes
    generate_instance_name();

    // Check network interface and ensure we have a valid IP
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "No STA network interface found!");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "No valid IP address! Cannot advertise mDNS service");
        return;
    }

    ESP_LOGI(TAG, "Current device IP: " IPSTR, IP2STR(&ip_info.ip));

    // Get port from configuration
    uint16_t service_port = lifecycle_get_port();

    ESP_LOGI(TAG, "Adding service: %s._scream._udp.local:%d", g_instance_name, service_port);

    // Remove any existing service first
    mdns_service_remove("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Determine mode and type
    bool enable_usb_sender = lifecycle_get_enable_usb_sender();
    bool enable_spdif_sender = lifecycle_get_enable_spdif_sender();
    const char *mode = (enable_usb_sender || enable_spdif_sender) ? "sender" : "receiver";
    const char *type = "unknown";
    if (enable_spdif_sender) {
        type = "spdif";
    } else if (enable_usb_sender) {
        type = "usb";
    } else {
        type = "receiver";
    }

    // Get battery level and MAC address
    get_battery_level_string(s_battery_level_str, sizeof(s_battery_level_str));
    uint8_t mac[6];
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (mac_err != ESP_OK) {
        mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (mac_err == ESP_OK) {
        snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strncpy(s_mac_str, "unknown", sizeof(s_mac_str));
    }

    // Prepare TXT records
    const char *samplerates = "44100,48000";
    const char *codecs = "lpcm";
    const char *channels = "2";

    mdns_txt_item_t txt_records[] = {
        {"mode", mode},
        {"type", type},
        {"battery", s_battery_level_str},
        {"mac", s_mac_str},
        {"samplerates", samplerates},
        {"codecs", codecs},
        {"channels", channels}
    };

    ESP_LOGI(TAG, "TXT: mode=%s, type=%s, battery=%s, mac=%s", mode, type, s_battery_level_str, s_mac_str);

    // Add service
    esp_err_t err = mdns_service_add(g_instance_name, "_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO,
                                     service_port, txt_records, sizeof(txt_records)/sizeof(txt_records[0]));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add mDNS service: %s", esp_err_to_name(err));
        return;
    }

    s_advertisement_enabled = true;
    ESP_LOGI(TAG, "mDNS service advertisement started successfully");

    // Start TXT record update task
    if (s_txt_update_task == NULL) {
        s_txt_task_running = true;
        BaseType_t ret = xTaskCreate(txt_update_task, "mdns_txt_update", 4096, NULL, 5, &s_txt_update_task);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create TXT update task");
            s_txt_task_running = false;
        } else {
            ESP_LOGI(TAG, "TXT record update task started");
        }
    }
}

/**
 * @brief Stop mDNS service advertisement
 */
void mdns_service_stop(void) {
    ESP_LOGI(TAG, "Stopping mDNS service advertisement");

    // Stop TXT update task
    if (s_txt_update_task != NULL) {
        s_txt_task_running = false;
        vTaskDelay(pdMS_TO_TICKS(100)); // Give task time to exit
        if (s_txt_update_task != NULL) {
            vTaskDelete(s_txt_update_task);
            s_txt_update_task = NULL;
        }
    }

    // Remove service advertisement
    if (s_advertisement_enabled) {
        esp_err_t err = mdns_service_remove("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove mDNS service: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "mDNS service removed");
        }
        s_advertisement_enabled = false;
    }
}

/**
 * @brief Deinitialize mDNS service (call during shutdown)
 */
void mdns_service_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing mDNS service");

    // Stop advertisement first
    mdns_service_stop();

    // Free mDNS
    if (s_mdns_initialized) {
        mdns_free();
        s_mdns_initialized = false;
        ESP_LOGI(TAG, "mDNS service deinitialized");
    }
}

/**
 * @brief Update the mDNS hostname without requiring a reboot
 */
esp_err_t mdns_service_update_hostname(void) {
    ESP_LOGI(TAG, "Updating mDNS hostname");

    if (!s_mdns_initialized) {
        ESP_LOGE(TAG, "mDNS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop current advertisement
    bool was_advertising = s_advertisement_enabled;
    if (was_advertising) {
        mdns_service_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Regenerate hostname and instance name
    generate_instance_name();

    // Update hostname in mDNS
    esp_err_t err = mdns_hostname_set(g_hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update hostname: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set(g_instance_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update instance name: %s", esp_err_to_name(err));
        return err;
    }

    // Restart advertisement if it was running
    if (was_advertising) {
        mdns_service_start();
    }

    ESP_LOGI(TAG, "mDNS hostname updated to '%s'", g_hostname);
    return ESP_OK;
}

/**
 * @brief Check if mDNS service is initialized
 */
bool mdns_service_is_initialized(void) {
    return s_mdns_initialized;
}

/**
 * @brief Check if service advertisement is enabled
 */
bool mdns_service_is_advertising(void) {
    return s_advertisement_enabled;
}
