#include "mdns_discovery.h"
#include "mdns_service.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <arpa/inet.h>
#include "lifecycle_manager.h"

static const char *TAG = "mdns_discovery";

// mDNS query constants
#define MDNS_QUERY_TIMEOUT_MS 3000
#define MDNS_MAX_RESULTS 10

// Discovery runtime state
static SemaphoreHandle_t s_device_mutex = NULL;
static discovered_device_t s_devices[MAX_DISCOVERED_DEVICES];
static size_t s_device_count = 0;
static volatile bool s_task_running = false;
static bool s_runtime_initialized = false;
static time_t s_last_query_time = 0;
static time_t s_last_cleanup_time = 0;
static time_t s_last_heartbeat_time = 0;

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
        if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Check if device already exists (by IP)
            bool device_exists = false;
            esp_ip4_addr_t ip4_addr = {0};

            // Get IPv4 address from result
            mdns_ip_addr_t *addr = r->addr;
            while (addr) {
                if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                    ip4_addr.addr = addr->addr.u_addr.ip4.addr;
                    ESP_LOGD(TAG, "    Found IPv4: %s", inet_ntoa(ip4_addr));
                    break;
                }
                addr = addr->next;
            }

            if (ip4_addr.addr != 0) {
                // Check if device exists
                for (size_t i = 0; i < s_device_count; i++) {
                    if (s_devices[i].ip_addr.addr == ip4_addr.addr) {
                        // Update existing device timestamp
                        s_devices[i].last_seen = current_time;
                        device_exists = true;
                        ESP_LOGD(TAG, "  Updated existing device: %s (%s)",
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
                    } else if (r->hostname) {
                        strncpy(new_device.hostname, r->hostname, sizeof(new_device.hostname) - 1);
                    } else {
                        snprintf(new_device.hostname, sizeof(new_device.hostname),
                                "scream_%08x", (unsigned int)ip4_addr.addr);
                    }
                    new_device.hostname[sizeof(new_device.hostname) - 1] = '\0';

                    // Set IP address
                    new_device.ip_addr = ip4_addr;

                    // Set port (use service port if available, otherwise default from config)
                    new_device.port = r->port ? r->port : lifecycle_get_port();

                    // Set last seen time
                    new_device.last_seen = current_time;

                    // Add to device list
                    s_devices[s_device_count++] = new_device;
                    ESP_LOGI(TAG, "*** NEW DEVICE: %s (%s:%d) ***",
                            new_device.hostname, inet_ntoa(new_device.ip_addr), new_device.port);
                    ESP_LOGI(TAG, "Total devices: %d", s_device_count);
                }
            }

            // Release mutex
            xSemaphoreGive(s_device_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to take device mutex!");
        }

        r = r->next;
    }
    ESP_LOGD(TAG, "=== RESULT PROCESSING COMPLETE ===");
}

// Periodic discovery worker
void mdns_discovery_tick(void) {
    if (!s_task_running) {
        return;
    }

    time_t current_time = time(NULL);
    if (current_time == ((time_t)-1)) {
        return;
    }

    const int QUERY_INTERVAL = 10;      // Send query every 10 seconds
    const int CLEANUP_INTERVAL = 30;    // Clean up stale devices every 30 seconds
    const int HEARTBEAT_INTERVAL = 30;  // Heartbeat every 30 seconds

    if (!s_runtime_initialized) {
        ESP_LOGI(TAG, "Starting discovery loop (query every %ds, cleanup every %ds)",
                 QUERY_INTERVAL, CLEANUP_INTERVAL);
        s_last_query_time = 0;
        s_last_cleanup_time = current_time;
        s_last_heartbeat_time = current_time;
        s_runtime_initialized = true;
    }

    // Heartbeat log
    if ((current_time - s_last_heartbeat_time) >= HEARTBEAT_INTERVAL) {
        ESP_LOGI(TAG, "[Heartbeat] Discovery active, %d device(s) found", (int)s_device_count);
        s_last_heartbeat_time = current_time;
    }

    // Send mDNS query at the configured interval
    if ((current_time - s_last_query_time) >= QUERY_INTERVAL) {
        ESP_LOGD(TAG, "Querying for _scream._udp services...");

        // Perform service discovery query for _scream._udp
        mdns_result_t *results = NULL;
        esp_err_t err = mdns_query_ptr("_scream", "_udp",
                                      MDNS_QUERY_TIMEOUT_MS, MDNS_MAX_RESULTS, &results);

        if (err == ESP_OK) {
            if (results) {
                int result_count = 0;
                mdns_result_t *r = results;
                while (r) {
                    result_count++;
                    r = r->next;
                }
                ESP_LOGI(TAG, "Found %d _scream._udp service(s)", result_count);
                process_mdns_results(results);
                mdns_query_results_free(results);
            } else {
                ESP_LOGD(TAG, "No _scream._udp services found");
            }
        } else {
            ESP_LOGW(TAG, "Query failed: %s", esp_err_to_name(err));
        }

        s_last_query_time = current_time;
    }

    // Periodic cleanup
    if ((current_time - s_last_cleanup_time) >= CLEANUP_INTERVAL) {
        ESP_LOGD(TAG, "Performing stale device cleanup");
        if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            remove_stale_devices();
            xSemaphoreGive(s_device_mutex);
        }
        s_last_cleanup_time = current_time;
    }
}

/**
 * @brief Start the continuous mDNS discovery task
 */
esp_err_t mdns_discovery_start(void) {
    ESP_LOGI(TAG, "Starting mDNS discovery");

    if (s_task_running) {
        ESP_LOGW(TAG, "Discovery already running");
        return ESP_OK;
    }

    // Check that mDNS service is initialized
    if (!mdns_service_is_initialized()) {
        ESP_LOGE(TAG, "mDNS service not initialized! Call mdns_service_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Create the device mutex
    if (s_device_mutex == NULL) {
        s_device_mutex = xSemaphoreCreateMutex();
        if (s_device_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create device mutex");
            return ESP_FAIL;
        }
    }

    // Initialize device count
    s_device_count = 0;

    // Set task running flag
    s_task_running = true;

    // Initialise runtime state so the next tick performs work immediately
    s_runtime_initialized = false;

    ESP_LOGI(TAG, "mDNS discovery worker enabled");
    return ESP_OK;
}

/**
 * @brief Stop the continuous mDNS discovery task
 */
esp_err_t mdns_discovery_stop(void) {
    ESP_LOGI(TAG, "Stopping mDNS discovery");

    if (!s_task_running) {
        ESP_LOGW(TAG, "Discovery not running");
        return ESP_OK;
    }

    // Set flag to stop worker
    s_task_running = false;
    s_runtime_initialized = false;

    // Delete the device mutex
    if (s_device_mutex != NULL) {
        vSemaphoreDelete(s_device_mutex);
        s_device_mutex = NULL;
    }

    // Clear device list
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));

    ESP_LOGI(TAG, "mDNS discovery stopped");
    return ESP_OK;
}

/**
 * @brief Get the list of discovered devices
 */
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

    ESP_LOGD(TAG, "Returning %d discovered device(s)", copy_count);
    return ESP_OK;
}