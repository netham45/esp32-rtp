#include "mdns_discovery.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "mdns.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <arpa/inet.h>
#include "esp_timer.h"
#include "lifecycle_manager.h"
#include "bq25895/bq25895_integration.h"

static const char *TAG = "network_discovery";

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

// mDNS constants
#define MDNS_SERVICE_NAME "scream"      // No underscore - mdns_service_add adds it
#define MDNS_SERVICE_PROTO "udp"        // No underscore - mdns_service_add adds it
#define MDNS_QUERY_TIMEOUT_MS 3000
#define MDNS_MAX_RESULTS 10

// Global instance name with MAC suffix
static char g_instance_name[32] = {0};
// Global hostname with MAC suffix
static char g_hostname[32] = {0};

// Task management variables for continuous discovery
static TaskHandle_t s_discovery_task = NULL;
static SemaphoreHandle_t s_device_mutex = NULL;
static discovered_device_t s_devices[MAX_DISCOVERED_DEVICES];
static size_t s_device_count = 0;
static volatile bool s_task_running = false;

// Reference counting for shared usage
static SemaphoreHandle_t s_ref_count_mutex = NULL;
static int s_ref_count = 0;

// Advertisement control
static bool s_advertisement_enabled = false;
static bool s_mdns_initialized = false;

// Helper function to remove stale devices (older than 2 minutes)
static void remove_stale_devices(void) {
    time_t current_time = time(NULL);
    const int STALE_TIMEOUT = 120; // 2 minutes in seconds
    
    size_t initial_count = s_device_count;
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < s_device_count; read_idx++) {
        if ((current_time - s_devices[read_idx].last_seen) < STALE_TIMEOUT) {
            // Device is not stale, keep it
            if (write_idx != read_idx) {
                s_devices[write_idx] = s_devices[read_idx];
            }
            write_idx++;
        } else {
            ESP_LOGD(TAG, "Removing stale device: %s (%s) - last seen %d seconds ago",
                     s_devices[read_idx].hostname,
                     inet_ntoa(s_devices[read_idx].ip_addr),
                     (int)(current_time - s_devices[read_idx].last_seen));
        }
    }
    s_device_count = write_idx;
    
    if (initial_count != s_device_count) {
        ESP_LOGD(TAG, "Removed %d stale device(s), %d active device(s) remaining",
                (int)(initial_count - s_device_count), (int)s_device_count);
    }
}

// Process mDNS query results and add to device list
static void process_mdns_results(mdns_result_t *results) {
    ESP_LOGD(TAG, "=== PROCESSING MDNS RESULTS ===");
    time_t current_time = time(NULL);
    mdns_result_t *r = results;
    int result_num = 0;
    
    while (r) {
        result_num++;
        ESP_LOGD(TAG, "Processing result #%d:", result_num);
        ESP_LOGD(TAG, "  Instance: %s", r->instance_name ? r->instance_name : "NULL");
        ESP_LOGD(TAG, "  Hostname: %s", r->hostname ? r->hostname : "NULL");
        ESP_LOGD(TAG, "  Port: %d", r->port);
        
        // Take mutex to access device list
        ESP_LOGD(TAG, "  Attempting to take device mutex...");
        if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGD(TAG, "  Got device mutex");
            
            // Check if device already exists (by IP)
            bool device_exists = false;
            esp_ip4_addr_t ip4_addr = {0};
            
            // Get IPv4 address from result
            mdns_ip_addr_t *addr = r->addr;
            int addr_count = 0;
            ESP_LOGD(TAG, "  Looking for IP addresses...");
            while (addr) {
                addr_count++;
                ESP_LOGD(TAG, "    Address #%d type: %s", addr_count,
                        addr->addr.type == ESP_IPADDR_TYPE_V4 ? "IPv4" : "IPv6");
                if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                    ip4_addr.addr = addr->addr.u_addr.ip4.addr;
                    ESP_LOGD(TAG, "    Found IPv4: %s", inet_ntoa(ip4_addr));
                    break;
                }
                addr = addr->next;
            }
            
            if (addr_count == 0) {
                ESP_LOGW(TAG, "  No IP addresses in result!");
            }
            
            if (ip4_addr.addr != 0) {
                // Check if device exists
                ESP_LOGD(TAG, "  Checking if device exists in list of %d devices", s_device_count);
                for (size_t i = 0; i < s_device_count; i++) {
                    if (s_devices[i].ip_addr.addr == ip4_addr.addr) {
                        // Update existing device timestamp
                        s_devices[i].last_seen = current_time;
                        device_exists = true;
                        ESP_LOGD(TAG, "  UPDATED existing device: %s (%s)",
                                s_devices[i].hostname, inet_ntoa(s_devices[i].ip_addr));
                        break;
                    }
                }
                
                // Add new device if not exists and there's room
                if (!device_exists && s_device_count < MAX_DISCOVERED_DEVICES) {
                    discovered_device_t new_device = {0};
                    
                    // Set hostname - use instance name if available, otherwise hostname
                    if (r->instance_name) {
                        strncpy(new_device.hostname, r->instance_name, sizeof(new_device.hostname) - 1);
                        ESP_LOGD(TAG, "  Using instance name: %s", r->instance_name);
                    } else if (r->hostname) {
                        strncpy(new_device.hostname, r->hostname, sizeof(new_device.hostname) - 1);
                        ESP_LOGD(TAG, "  Using hostname: %s", r->hostname);
                    } else {
                        snprintf(new_device.hostname, sizeof(new_device.hostname),
                                "scream_%08x", (unsigned int)ip4_addr.addr);
                        ESP_LOGD(TAG, "  Generated hostname: %s", new_device.hostname);
                    }
                    new_device.hostname[sizeof(new_device.hostname) - 1] = '\0';
                    
                    // Set IP address
                    new_device.ip_addr = ip4_addr;
                    
                    // Set port (use service port if available, otherwise default from config)
                    new_device.port = r->port ? r->port : lifecycle_get_port();
                    ESP_LOGD(TAG, "  Using port: %d", new_device.port);
                    
                    // Set last seen time
                    new_device.last_seen = current_time;
                    
                    // Add to device list
                    s_devices[s_device_count++] = new_device;
                    ESP_LOGD(TAG, "  *** NEW DEVICE ADDED: %s (%s:%d) ***",
                            new_device.hostname, inet_ntoa(new_device.ip_addr), new_device.port);
                    ESP_LOGD(TAG, "  Total devices now: %d", s_device_count);
                } else if (!device_exists) {
                    ESP_LOGW(TAG, "  Cannot add device - list full (%d/%d)",
                            s_device_count, MAX_DISCOVERED_DEVICES);
                }
            } else {
                ESP_LOGW(TAG, "  Skipping result - no valid IPv4 address");
            }
            
            // Release mutex
            xSemaphoreGive(s_device_mutex);
            ESP_LOGD(TAG, "  Device mutex released");
        } else {
            ESP_LOGE(TAG, "  FAILED to take device mutex!");
        }
        
        r = r->next;
    }
    ESP_LOGD(TAG, "=== RESULT PROCESSING COMPLETE ===");
}

// Main discovery task function for continuous mDNS discovery
static void mdns_discovery_task(void *pvParameters) {
    ESP_LOGD(TAG, "========================================");
    ESP_LOGD(TAG, "mDNS DISCOVERY TASK STARTED!");
    ESP_LOGD(TAG, "Task handle: %p", xTaskGetCurrentTaskHandle());
    ESP_LOGD(TAG, "Stack high water mark: %lu", uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGD(TAG, "s_task_running = %s", s_task_running ? "TRUE" : "FALSE");
    ESP_LOGD(TAG, "s_mdns_initialized = %s", s_mdns_initialized ? "TRUE" : "FALSE");
    ESP_LOGD(TAG, "s_advertisement_enabled = %s", s_advertisement_enabled ? "TRUE" : "FALSE");
    ESP_LOGD(TAG, "========================================");
    
    // CRITICAL DEBUG: List all network interfaces
    ESP_LOGD(TAG, "CHECKING ALL NETWORK INTERFACES:");
    esp_netif_t *netif = NULL;
    int netif_count = 0;
    while ((netif = esp_netif_next(netif)) != NULL) {
        netif_count++;
        const char *key = esp_netif_get_ifkey(netif);
        const char *desc = esp_netif_get_desc(netif);
        esp_netif_ip_info_t ip_info;
        esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
        ESP_LOGD(TAG, "Interface #%d: key='%s', desc='%s'", netif_count, key, desc);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "  IP: " IPSTR ", Netmask: " IPSTR ", GW: " IPSTR,
                    IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
        } else {
            ESP_LOGD(TAG, "  No IP info available");
        }
    }
    ESP_LOGD(TAG, "Total network interfaces: %d", netif_count);
    
    // Timing variables
    time_t last_query_time = 0;
    time_t last_cleanup_time = time(NULL);
    time_t last_heartbeat = time(NULL);
    time_t last_txt_update = time(NULL);
    const int QUERY_INTERVAL = 10;  // Send query every 10 seconds
    const int CLEANUP_INTERVAL = 30; // Clean up stale devices every 30 seconds
    const int HEARTBEAT_INTERVAL = 5; // Heartbeat every 5 seconds
    const int TXT_UPDATE_INTERVAL = 30; // Update TXT records every 30 seconds
    
    ESP_LOGD(TAG, "Starting main discovery loop...");
    ESP_LOGD(TAG, "Query interval: %d sec, Cleanup interval: %d sec, Heartbeat: %d sec",
            QUERY_INTERVAL, CLEANUP_INTERVAL, HEARTBEAT_INTERVAL);
    
    int iteration = 0;
    
    // Main task loop
    while (s_task_running) {
        iteration++;
        time_t current_time = time(NULL);
        
        // Heartbeat log every 5 seconds
        if ((current_time - last_heartbeat) >= HEARTBEAT_INTERVAL) {
            ESP_LOGD(TAG, "[HEARTBEAT] Task alive! Iteration %d, devices: %d, running: %s, adv: %s",
                    iteration, s_device_count,
                    s_task_running ? "YES" : "NO",
                    s_advertisement_enabled ? "YES" : "NO");
            
            // Log network info
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    ESP_LOGD(TAG, "[HEARTBEAT] Current IP: " IPSTR, IP2STR(&ip_info.ip));
                }
            }
            last_heartbeat = current_time;
        }
        
        // Send mDNS query every 10 seconds
        if ((current_time - last_query_time) >= QUERY_INTERVAL) {
            ESP_LOGD(TAG, "============ MDNS QUERY START ============");
            ESP_LOGD(TAG, "Query #%d for service: _scream._udp", iteration/10 + 1);
            ESP_LOGD(TAG, "Current active devices: %d", s_device_count);
            ESP_LOGD(TAG, "Timeout: %d ms, Max results: %d", MDNS_QUERY_TIMEOUT_MS, MDNS_MAX_RESULTS);
            
            // Check if our service is registered before querying
            if (s_advertisement_enabled) {
                ESP_LOGD(TAG, "Our device is advertising _scream._udp service");
            } else {
                ESP_LOGW(TAG, "WARNING: Our service is NOT advertising!");
            }
            
            // Perform service discovery query for _scream._udp
            mdns_result_t *results = NULL;
            ESP_LOGD(TAG, "Calling mdns_query_ptr('_scream', '_udp', %d ms timeout, max %d results)...",
                    MDNS_QUERY_TIMEOUT_MS, MDNS_MAX_RESULTS);
            ESP_LOGD(TAG, "*** ABOUT TO CALL mdns_query_ptr() - DISCOVERING SCREAM SERVICES ***");
            
            // Add a timestamp before the call
            int64_t start_time = esp_timer_get_time();
            ESP_LOGD(TAG, "Query start time: %lld us", start_time);
            
            // Query for all _scream._udp services on the network
            esp_err_t err = mdns_query_ptr("_scream", "_udp",
                                          MDNS_QUERY_TIMEOUT_MS, MDNS_MAX_RESULTS, &results);
            
            int64_t end_time = esp_timer_get_time();
            int64_t duration_ms = (end_time - start_time) / 1000;
            ESP_LOGD(TAG, "Query end time: %lld us (duration: %lld ms)", end_time, duration_ms);
            
            if (duration_ms > MDNS_QUERY_TIMEOUT_MS + 500) {
                ESP_LOGE(TAG, "*** WARNING: Query took %lld ms but timeout was %d ms! ***",
                        duration_ms, MDNS_QUERY_TIMEOUT_MS);
                ESP_LOGE(TAG, "*** mDNS system appears to be HANGING! ***");
            }
            
            ESP_LOGD(TAG, "mdns_query_ptr() RETURNED! Result: %s (0x%x)", esp_err_to_name(err), err);
            
            if (err == ESP_OK) {
                if (results) {
                    int result_count = 0;
                    mdns_result_t *r = results;
                    while (r) {
                        result_count++;
                        r = r->next;
                    }
                    ESP_LOGD(TAG, "Service discovery SUCCESS! Found %d _scream._udp service(s)", result_count);
                    ESP_LOGD(TAG, "Processing service results...");
                    process_mdns_results(results);
                    ESP_LOGD(TAG, "Freeing results...");
                    mdns_query_results_free(results);
                    ESP_LOGD(TAG, "Results freed");
                } else {
                    ESP_LOGD(TAG, "Query returned OK but NO _scream._udp services found");
                }
            } else {
                ESP_LOGE(TAG, "QUERY FAILED! Error: %s (0x%x)", esp_err_to_name(err), err);
            }
            ESP_LOGD(TAG, "============ MDNS QUERY END ============");
            
            last_query_time = current_time;
        }
        
        // Periodic cleanup every 30 seconds
        if ((current_time - last_cleanup_time) >= CLEANUP_INTERVAL) {
            ESP_LOGD(TAG, "--- Cleanup check (iteration %d) ---", iteration);
            if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGD(TAG, "Got mutex, performing stale device cleanup");
                remove_stale_devices();
                xSemaphoreGive(s_device_mutex);
                ESP_LOGD(TAG, "Cleanup complete, mutex released");
            } else {
                ESP_LOGW(TAG, "Failed to get mutex for cleanup!");
            }
            last_cleanup_time = current_time;
        }
        
        // Update TXT records periodically (every 30 seconds)
        if (s_advertisement_enabled && (current_time - last_txt_update) >= TXT_UPDATE_INTERVAL) {
            ESP_LOGD(TAG, "--- Updating TXT records (iteration %d) ---", iteration);

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

            // Get current battery level
            char battery_level_str[8];
            get_battery_level_string(battery_level_str, sizeof(battery_level_str));

            // Get MAC address for TXT record
            uint8_t mac[6];
            char mac_str[18]; // xx:xx:xx:xx:xx:xx + null terminator
            esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
            if (mac_err != ESP_OK) {
                mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
            }
            if (mac_err == ESP_OK) {
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                strncpy(mac_str, "unknown", sizeof(mac_str));
            }

            // Hard-coded values that don't change
            const char *samplerates = "44100,48000";
            const char *codecs = "lpcm";
            const char *channels = "2";

            // Prepare updated TXT records
            mdns_txt_item_t txt_records[] = {
                {"mode", mode},
                {"type", type},
                {"battery", battery_level_str},
                {"mac", mac_str},
                {"samplerates", samplerates},
                {"codecs", codecs},
                {"channels", channels}
            };
            
            // Update the TXT records for the service
            esp_err_t err = mdns_service_txt_set("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO,
                                                  txt_records, sizeof(txt_records)/sizeof(txt_records[0]));
            
            if (err == ESP_OK) {
                ESP_LOGD(TAG, "✓ TXT records updated successfully");
                ESP_LOGD(TAG, "  battery=%s%% (updated)", battery_level_str);
                ESP_LOGD(TAG, "  mode=%s, type=%s", mode, type);
            } else {
                ESP_LOGW(TAG, "Failed to update TXT records: %s", esp_err_to_name(err));
            }
            
            last_txt_update = current_time;
        }
        
        // Sleep for 1 second between iterations
        ESP_LOGD(TAG, "Loop iteration %d complete, sleeping 1 second...", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Cleanup on exit
    ESP_LOGD(TAG, "mDNS discovery task stopping");
    
    // Clear task handle
    s_discovery_task = NULL;
    
    // Delete self
    vTaskDelete(NULL);
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

esp_err_t mdns_discovery_start(void) {
    ESP_LOGD(TAG, "########################################");
    ESP_LOGD(TAG, "#### MDNS_DISCOVERY_START CALLED ####");
    ESP_LOGD(TAG, "########################################");
    
    // Generate instance name with MAC suffix if not already done
    if (g_instance_name[0] == '\0') {
        generate_instance_name();
    }
    
    ESP_LOGD(TAG, "Current state before start:");
    ESP_LOGD(TAG, "  s_mdns_initialized: %s", s_mdns_initialized ? "TRUE" : "FALSE");
    ESP_LOGD(TAG, "  s_task_running: %s", s_task_running ? "TRUE" : "FALSE");
    ESP_LOGD(TAG, "  s_advertisement_enabled: %s", s_advertisement_enabled ? "TRUE" : "FALSE");
    ESP_LOGD(TAG, "  s_ref_count: %d", s_ref_count);
    ESP_LOGD(TAG, "  s_device_count: %d", s_device_count);
    ESP_LOGD(TAG, "  s_discovery_task: %p", s_discovery_task);
    
    // Log network status
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_LOGD(TAG, "Network IP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            ESP_LOGW(TAG, "Failed to get IP info");
        }
    } else {
        ESP_LOGW(TAG, "No STA network interface found");
    }
    
    ESP_LOGD(TAG, "Starting continuous mDNS discovery");
    
    // Create ref count mutex if not exists
    ESP_LOGD(TAG, "Checking ref count mutex...");
    if (s_ref_count_mutex == NULL) {
        ESP_LOGD(TAG, "Creating ref count mutex...");
        s_ref_count_mutex = xSemaphoreCreateMutex();
        if (s_ref_count_mutex == NULL) {
            ESP_LOGE(TAG, "FAILED to create ref count mutex!");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Ref count mutex created successfully");
    } else {
        ESP_LOGD(TAG, "Ref count mutex already exists");
    }
    
    // Take ref count mutex
    ESP_LOGD(TAG, "Taking ref count mutex...");
    if (xSemaphoreTake(s_ref_count_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "FAILED to take ref count mutex!");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Got ref count mutex");
    
    // Increment reference count
    s_ref_count++;
    ESP_LOGD(TAG, "Reference count incremented: %d -> %d", s_ref_count - 1, s_ref_count);
    
    // If this is the first reference, start the task
    if (s_ref_count == 1) {
        ESP_LOGD(TAG, "First reference - initializing mDNS system");
        
        // Initialize esp_mdns if not already done
        if (!s_mdns_initialized) {
            ESP_LOGD(TAG, "########################################");
            ESP_LOGD(TAG, ">>> INITIALIZING ESP_MDNS SYSTEM <<<");
            ESP_LOGD(TAG, "########################################");
            
            ESP_LOGD(TAG, "Calling mdns_init()...");
            esp_err_t err = mdns_init();
            ESP_LOGD(TAG, "mdns_init() returned: %s (0x%x)", esp_err_to_name(err), err);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "*** CRITICAL: mdns_init() FAILED! ***");
                ESP_LOGE(TAG, "Error details: %s (code: 0x%x)", esp_err_to_name(err), err);
                s_ref_count--;
                xSemaphoreGive(s_ref_count_mutex);
                return ESP_FAIL;
            }
            ESP_LOGD(TAG, "✓ mdns_init() SUCCESSFUL");
            
            // Set hostname with MAC suffix
            ESP_LOGD(TAG, "Setting hostname to '%s'...", g_hostname);
            err = mdns_hostname_set(g_hostname);
            ESP_LOGD(TAG, "mdns_hostname_set() returned: %s (0x%x)", esp_err_to_name(err), err);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "WARNING: Failed to set hostname!");
            } else {
                ESP_LOGD(TAG, "✓ Hostname set successfully to '%s'", g_hostname);
            }
            
            ESP_LOGD(TAG, "Setting instance name to '%s'...", g_instance_name);
            err = mdns_instance_name_set(g_instance_name);
            ESP_LOGD(TAG, "mdns_instance_name_set() returned: %s (0x%x)", esp_err_to_name(err), err);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "WARNING: Failed to set instance name!");
            } else {
                ESP_LOGD(TAG, "✓ Instance name set successfully to '%s'", g_instance_name);
            }
            
            // Add the mDNS service immediately after init (like official example)
            ESP_LOGD(TAG, "########################################");
            ESP_LOGD(TAG, ">>> ADDING MDNS SERVICE NOW <<<");
            ESP_LOGD(TAG, "########################################");
            
            // Get port from configuration
            uint16_t service_port = lifecycle_get_port();
            ESP_LOGD(TAG, "Adding service: _%s._%s on port %d", MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO, service_port);
            
            // Prepare TXT records
            // Determine mode (sender or receiver)
            bool enable_usb_sender = lifecycle_get_enable_usb_sender();
            bool enable_spdif_sender = lifecycle_get_enable_spdif_sender();
            const char *mode = (enable_usb_sender || enable_spdif_sender) ? "sender" : "receiver";
            
            // Determine type (spdif or usb)
            const char *type = "unknown";
            if (enable_spdif_sender) {
                type = "spdif";
            } else if (enable_usb_sender) {
                type = "usb";
            } else {
                type = "receiver";
            }
            
            // Get battery level from BQ25895 if available
            char battery_level_str[8];
            get_battery_level_string(battery_level_str, sizeof(battery_level_str));

            // Get MAC address for TXT record
            uint8_t mac[6];
            char mac_str[18]; // xx:xx:xx:xx:xx:xx + null terminator
            esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
            if (mac_err != ESP_OK) {
                mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
            }
            if (mac_err == ESP_OK) {
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                strncpy(mac_str, "unknown", sizeof(mac_str));
            }

            // Hard-coded supported sample rates, codecs, and channels
            const char *samplerates = "44100,48000";
            const char *codecs = "lpcm";
            const char *channels = "2";

            mdns_txt_item_t txt_records[] = {
                {"mode", mode},
                {"type", type},
                {"battery", battery_level_str},
                {"mac", mac_str},
                {"samplerates", samplerates},
                {"codecs", codecs},
                {"channels", channels}
            };
            
            ESP_LOGD(TAG, "TXT Records:");
            ESP_LOGD(TAG, "  mode=%s", mode);
            ESP_LOGD(TAG, "  type=%s", type);
            ESP_LOGD(TAG, "  battery=%s", battery_level_str);
            ESP_LOGD(TAG, "  mac=%s", mac_str);
            ESP_LOGD(TAG, "  samplerates=%s", samplerates);
            ESP_LOGD(TAG, "  codecs=%s", codecs);
            ESP_LOGD(TAG, "  channels=%s", channels);
            
            // Add service immediately after mdns_init (following official example pattern)
            err = mdns_service_add(g_instance_name, "_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO,
                                  service_port, txt_records, sizeof(txt_records)/sizeof(txt_records[0]));
            ESP_LOGD(TAG, "mdns_service_add() returned: %s (0x%x)", esp_err_to_name(err), err);
            
            if (err == ESP_OK) {
                s_advertisement_enabled = true;
                ESP_LOGD(TAG, "✓✓✓ Service added successfully!");
                ESP_LOGD(TAG, "Service advertising as: %s._%s._%s.local",
                        g_instance_name, MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO);
            } else {
                ESP_LOGE(TAG, "*** FAILED to add mDNS service! ***");
                ESP_LOGE(TAG, "This is critical - service won't be discoverable");
            }
            
            s_mdns_initialized = true;
            ESP_LOGD(TAG, "########################################");
            ESP_LOGD(TAG, ">>> mDNS SYSTEM INITIALIZED <<<");
            ESP_LOGD(TAG, "########################################");
            
            ESP_LOGD(TAG, "mDNS initialized with service advertisement");
        } else {
            ESP_LOGD(TAG, "mDNS already initialized (s_mdns_initialized = true)");
        }
        
        // Create the device mutex
        ESP_LOGD(TAG, "Creating device mutex...");
        s_device_mutex = xSemaphoreCreateMutex();
        if (s_device_mutex == NULL) {
            ESP_LOGE(TAG, "FAILED to create device mutex!");
            if (s_mdns_initialized) {
                ESP_LOGD(TAG, "Cleaning up: calling mdns_free()...");
                mdns_free();
                s_mdns_initialized = false;
            }
            s_ref_count--;
            xSemaphoreGive(s_ref_count_mutex);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "✓ Device mutex created successfully");
        
        // Initialize device count
        ESP_LOGD(TAG, "Initializing device count to 0");
        s_device_count = 0;
        
        // Set task running flag
        ESP_LOGD(TAG, "Setting s_task_running = true");
        s_task_running = true;
        
        // Create the discovery task
        ESP_LOGD(TAG, "Creating mDNS discovery task...");
        ESP_LOGD(TAG, "  Task name: mdns_discovery");
        ESP_LOGD(TAG, "  Stack size: 4096");
        ESP_LOGD(TAG, "  Priority: 5");
        
        BaseType_t ret = xTaskCreatePinnedToCore(mdns_discovery_task,
                                      "mdns_discovery",
                                      4096,
                                      NULL,
                                      1,
                                      &s_discovery_task, 0);
        
        ESP_LOGD(TAG, "xTaskCreate() returned: %s", ret == pdPASS ? "pdPASS" : "pdFAIL");
        
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "*** CRITICAL: Failed to create discovery task! ***");
            ESP_LOGD(TAG, "Cleaning up resources...");
            vSemaphoreDelete(s_device_mutex);
            s_device_mutex = NULL;
            if (s_mdns_initialized) {
                ESP_LOGD(TAG, "Calling mdns_free()...");
                mdns_free();
                s_mdns_initialized = false;
            }
            s_task_running = false;
            s_ref_count--;
            xSemaphoreGive(s_ref_count_mutex);
            return ESP_FAIL;
        }
        
        ESP_LOGD(TAG, "✓ mDNS discovery task created successfully");
        ESP_LOGD(TAG, "Task handle: %p", s_discovery_task);
        
        // Service was already added during mdns_init
        ESP_LOGD(TAG, "mDNS service already configured during initialization");
    } else {
        ESP_LOGD(TAG, "mDNS discovery already running (ref count: %d)", s_ref_count);
        
    }
    
    xSemaphoreGive(s_ref_count_mutex);
    
    ESP_LOGD(TAG, "==== MDNS_DISCOVERY_START COMPLETE (ref count: %d) ====", s_ref_count);
    return ESP_OK;
}

esp_err_t mdns_discovery_stop(void) {
    ESP_LOGD(TAG, "Stopping continuous mDNS discovery");
    
    if (s_ref_count_mutex == NULL) {
        ESP_LOGW(TAG, "mDNS discovery not initialized");
        return ESP_OK;
    }
    
    // Take ref count mutex
    if (xSemaphoreTake(s_ref_count_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take ref count mutex");
        return ESP_FAIL;
    }
    
    // Decrement reference count
    if (s_ref_count > 0) {
        s_ref_count--;
        ESP_LOGD(TAG, "mDNS reference count decremented to %d", s_ref_count);
    }
    
    // If this was the last reference, stop the task
    if (s_ref_count == 0) {
        ESP_LOGD(TAG, "Last reference removed, stopping mDNS task (currently tracking %d device(s))", s_device_count);
        
        // Set flag to stop task
        s_task_running = false;
        
        // Wait a bit for task to notice the flag and exit
        if (s_discovery_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(2000)); // Give task time to exit cleanly
            
            // If task still exists, delete it
            if (s_discovery_task != NULL) {
                vTaskDelete(s_discovery_task);
                s_discovery_task = NULL;
            }
        }
        
        // Remove service advertisement if it was enabled
        if (s_advertisement_enabled) {
            mdns_service_remove("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO);
            s_advertisement_enabled = false;
        }
        
        // Free mDNS
        if (s_mdns_initialized) {
            mdns_free();
            s_mdns_initialized = false;
        }
        
        // Delete the device mutex
        if (s_device_mutex != NULL) {
            vSemaphoreDelete(s_device_mutex);
            s_device_mutex = NULL;
        }
        
        // Clear device list
        s_device_count = 0;
        memset(s_devices, 0, sizeof(s_devices));
        
        ESP_LOGD(TAG, "mDNS discovery task stopped");
    }
    
    xSemaphoreGive(s_ref_count_mutex);
    
    ESP_LOGD(TAG, "mDNS discovery stop complete (ref count: %d)", s_ref_count);
    return ESP_OK;
}

esp_err_t mdns_discovery_get_devices(discovered_device_t *out_devices, size_t max_devices, size_t *out_count) {
    if (out_devices == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_device_mutex == NULL) {
        ESP_LOGW(TAG, "Discovery not started");
        *out_count = 0;
        return ESP_OK;
    }
    
    // Take mutex with timeout
    if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take device mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    // Remove stale devices first
    remove_stale_devices();
    
    // Copy devices to output
    size_t copy_count = (s_device_count < max_devices) ? s_device_count : max_devices;
    memcpy(out_devices, s_devices, copy_count * sizeof(discovered_device_t));
    *out_count = copy_count;
    
    // Release mutex
    xSemaphoreGive(s_device_mutex);
    
    ESP_LOGD(TAG, "Returning %d active discovered device(s)", copy_count);
    return ESP_OK;
}

/**
 * @brief Enable or disable mDNS advertisement
 */
void mdns_discovery_enable_advertisement(bool enable) {
    ESP_LOGD(TAG, "########################################");
    ESP_LOGD(TAG, "#### ADVERTISEMENT %s REQUEST ####", enable ? "ENABLE" : "DISABLE");
    ESP_LOGD(TAG, "########################################");
    
    // Always regenerate instance name to pick up any hostname changes
    generate_instance_name();
    
    ESP_LOGD(TAG, "Current advertisement state: %s", s_advertisement_enabled ? "ENABLED" : "DISABLED");
    ESP_LOGD(TAG, "mDNS initialized: %s", s_mdns_initialized ? "YES" : "NO");
    
    if (!s_mdns_initialized) {
        ESP_LOGE(TAG, "*** ERROR: mDNS not initialized! ***");
        ESP_LOGE(TAG, "Cannot %s advertisement without mDNS init", enable ? "enable" : "disable");
        return;
    }
    
    // Check network interface and ensure we have a valid IP
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "*** ERROR: No STA network interface found! ***");
        return;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "*** ERROR: No valid IP address! Cannot advertise mDNS service ***");
        ESP_LOGE(TAG, "IP address is: " IPSTR, IP2STR(&ip_info.ip));
        return;
    }
    
    ESP_LOGD(TAG, "Current device IP: " IPSTR, IP2STR(&ip_info.ip));
    
    if (enable && !s_advertisement_enabled) {
        ESP_LOGD(TAG, "########################################");
        ESP_LOGD(TAG, ">>> ENABLING MDNS ADVERTISEMENT <<<");
        ESP_LOGD(TAG, "########################################");
        
        // Get port from configuration
        uint16_t service_port = lifecycle_get_port();
        
        ESP_LOGD(TAG, "Service parameters:");
        ESP_LOGD(TAG, "  Instance name: '%s'", g_instance_name);
        ESP_LOGD(TAG, "  Service type:  '_%s'", MDNS_SERVICE_NAME);
        ESP_LOGD(TAG, "  Protocol:      '_%s'", MDNS_SERVICE_PROTO);
        ESP_LOGD(TAG, "  Port:          %d", service_port);
        ESP_LOGD(TAG, "Full service name: %s._%s._%s.local",
                g_instance_name, MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO);
        
        // First, try to remove any existing service (in case of stale registration)
        ESP_LOGD(TAG, "Removing any existing service registration...");
        mdns_service_remove("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO);
        
        // Wait a bit for removal to take effect
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Prepare TXT records
        // Determine mode (sender or receiver)
        bool enable_usb_sender = lifecycle_get_enable_usb_sender();
        bool enable_spdif_sender = lifecycle_get_enable_spdif_sender();
        const char *mode = (enable_usb_sender || enable_spdif_sender) ? "sender" : "receiver";
        
        // Determine type (spdif or usb)
        const char *type = "unknown";
        if (enable_spdif_sender) {
            type = "spdif";
        } else if (enable_usb_sender) {
            type = "usb";
        } else {
            type = "receiver";
        }
        
        // Get battery level from BQ25895 if available
        char battery_level_str[8];
        get_battery_level_string(battery_level_str, sizeof(battery_level_str));

        // Get MAC address for TXT record
        uint8_t mac[6];
        char mac_str[18]; // xx:xx:xx:xx:xx:xx + null terminator
        esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
        if (mac_err != ESP_OK) {
            mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        }
        if (mac_err == ESP_OK) {
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            strncpy(mac_str, "unknown", sizeof(mac_str));
        }

        // Hard-coded supported sample rates, codecs, and channels
        const char *samplerates = "44100,48000";
        const char *codecs = "lpcm";
        const char *channels = "2";

        mdns_txt_item_t txt_records[] = {
            {"mode", mode},
            {"type", type},
            {"battery", battery_level_str},
            {"mac", mac_str},
            {"samplerates", samplerates},
            {"codecs", codecs},
            {"channels", channels}
        };
        
        ESP_LOGD(TAG, "TXT Records:");
        ESP_LOGD(TAG, "  mode=%s", mode);
        ESP_LOGD(TAG, "  type=%s", type);
        ESP_LOGD(TAG, "  battery=%s", battery_level_str);
        ESP_LOGD(TAG, "  mac=%s", mac_str);
        ESP_LOGD(TAG, "  samplerates=%s", samplerates);
        ESP_LOGD(TAG, "  codecs=%s", codecs);
        ESP_LOGD(TAG, "  channels=%s", channels);
        
        // Add service with explicit network interface binding
        ESP_LOGD(TAG, "Adding mDNS service...");
        esp_err_t err = mdns_service_add(g_instance_name, "_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO,
                                         service_port, txt_records, sizeof(txt_records)/sizeof(txt_records[0]));
        ESP_LOGD(TAG, "mdns_service_add() returned: %s (0x%x)", esp_err_to_name(err), err);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "*** FAILED to add mDNS service! ***");
            ESP_LOGE(TAG, "Error: %s (code: 0x%x)", esp_err_to_name(err), err);
            if (err == ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "Invalid state - mDNS may not be properly initialized");
            } else if (err == ESP_ERR_NO_MEM) {
                ESP_LOGE(TAG, "Out of memory");
            } else if (err == ESP_ERR_INVALID_ARG) {
                ESP_LOGE(TAG, "Invalid arguments provided");
            }
        } else {
            s_advertisement_enabled = true;
            ESP_LOGD(TAG, "########################################");
            ESP_LOGD(TAG, "✓✓✓ SERVICE ADDED SUCCESSFULLY ✓✓✓");
            ESP_LOGD(TAG, "########################################");
            ESP_LOGD(TAG, "Service is now advertising:");
            ESP_LOGD(TAG, "  Full name: %s._%s._%s.local", g_instance_name, MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO);
            ESP_LOGD(TAG, "  Service type: _%s._%s.local", MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO);
            ESP_LOGD(TAG, "  Instance: %s", g_instance_name);
            ESP_LOGD(TAG, "  Port: %d", service_port);
            ESP_LOGD(TAG, "Other devices should discover this as '_scream._udp'");
            
            // Update the instance name to force a re-announcement
            ESP_LOGD(TAG, "Updating instance name to force announcement...");
            mdns_service_instance_name_set(MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO, g_instance_name);
            
            // Wait longer for the service to propagate
            ESP_LOGD(TAG, "Waiting 500ms for service to propagate...");
            vTaskDelay(pdMS_TO_TICKS(500));
            
            // Test if we can query ourselves with SHORT timeout
            ESP_LOGD(TAG, "########################################");
            ESP_LOGD(TAG, "SELF-TEST: Querying our own service with 500ms timeout...");
            ESP_LOGD(TAG, "########################################");
            
            mdns_result_t *test_results = NULL;
            int64_t test_start = esp_timer_get_time();
            ESP_LOGD(TAG, "Self-test query starting at %lld us", test_start);
            
            esp_err_t test_err = mdns_query_ptr("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO,
                                               500, 1, &test_results);  // 500ms timeout
            
            int64_t test_end = esp_timer_get_time();
            ESP_LOGD(TAG, "Self-test query completed in %lld ms", (test_end - test_start) / 1000);
            
            if (test_err == ESP_OK) {
                if (test_results) {
                    ESP_LOGD(TAG, "✓✓✓ SELF-TEST SUCCESS: Found our own service!");
                    mdns_query_results_free(test_results);
                } else {
                    ESP_LOGE(TAG, "*** SELF-TEST FAILED: Query OK but no results!");
                    ESP_LOGE(TAG, "*** This means mDNS responder is NOT working!");
                }
            } else if (test_err == ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "*** SELF-TEST TIMEOUT: Query hung for 500ms!");
                ESP_LOGE(TAG, "*** mDNS system appears to be broken!");
            } else {
                ESP_LOGE(TAG, "*** SELF-TEST ERROR: %s (0x%x)", esp_err_to_name(test_err), test_err);
            }
            ESP_LOGD(TAG, "########################################");
        }
    } else if (!enable && s_advertisement_enabled) {
        ESP_LOGD(TAG, "Disabling mDNS advertisement...");
        // Remove service
        ESP_LOGD(TAG, "Calling mdns_service_remove('_%s', '_%s')...", MDNS_SERVICE_NAME, MDNS_SERVICE_PROTO);
        esp_err_t err = mdns_service_remove("_" MDNS_SERVICE_NAME, "_" MDNS_SERVICE_PROTO);
        ESP_LOGD(TAG, "mdns_service_remove() returned: %s (0x%x)", esp_err_to_name(err), err);
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove mDNS service: %s", esp_err_to_name(err));
        } else {
            s_advertisement_enabled = false;
            ESP_LOGD(TAG, "✓ mDNS service advertisement disabled");
        }
    } else {
        ESP_LOGD(TAG, "No action needed:");
        ESP_LOGD(TAG, "  Requested: %s", enable ? "ENABLE" : "DISABLE");
        ESP_LOGD(TAG, "  Current state: %s", s_advertisement_enabled ? "ENABLED" : "DISABLED");
    }
    
    ESP_LOGD(TAG, "########################################");
    ESP_LOGD(TAG, "#### ADVERTISEMENT REQUEST COMPLETE ####");
    ESP_LOGD(TAG, "########################################");
}