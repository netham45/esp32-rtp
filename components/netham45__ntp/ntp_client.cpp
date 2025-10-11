#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "ntp_client.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "time.h"
#include "mdns.h"

// IP address formatting macro
#ifndef IP2STR
#define IP2STR(addr) ((uint8_t*)&(addr))[0], ((uint8_t*)&(addr))[1], ((uint8_t*)&(addr))[2], ((uint8_t*)&(addr))[3]
#endif

#define QUERY_TARGET "screamrouter"  // Will query for screamrouter.local
#define MAX_FAILURE_COUNT 3          // Maximum number of consecutive failures before invalidating the cache
#define MDNS_CHECK_INTERVAL_MS 5000  // Check mDNS every 5 seconds
#define MDNS_QUERY_TIMEOUT_MS 3000   // mDNS query timeout
#define NTP_SERVER_PORT 123          // NTP port

// NTP micro-probe configuration
#define NTP_PROBE_BURST_SIZE 8       // Number of samples per burst
#define NTP_PROBE_TIMEOUT_MS 100     // Timeout per probe

static const char *TAG = "ntp_client";

// Task handle for NTP client
static TaskHandle_t ntp_client_task_handle = NULL;
static bool ntp_client_initialized = false;
static bool sntp_initialized = false;

// DNS cache structure
typedef struct {
    char ip_address[46];  // Cached IP address
    bool valid;           // Whether the cache is valid
    int failure_count;    // Count of consecutive failures
} dns_cache_t;

static dns_cache_t dns_cache = {
    .ip_address = {0},
    .valid = false,
    .failure_count = 0
};

// Time tracking for variance detection
static struct timeval last_known_time = {0, 0};
static int64_t last_known_time_us = 0;  // Timestamp in microseconds when last_known_time was captured

// ============================================================================
// NTP MICRO-PROBE & PLL for precision audio synchronization
// ============================================================================

// Simple PLL state for local_mono ≈ a * master_us + b
typedef struct {
    double a;           // Skew factor (≈ 1.0 + small ppm error)
    double b;           // Offset in microseconds
    int64_t last_update_mono_us;  // When PLL was last updated (local monotonic)
    int64_t last_master_us;       // Last master time used for update
    bool valid;         // Whether PLL has been initialized
    int sample_count;   // Number of samples incorporated
} ntp_pll_t;

static ntp_pll_t pll = {
    .a = 1.0,
    .b = 0.0,
    .last_update_mono_us = 0,
    .last_master_us = 0,
    .valid = false,
    .sample_count = 0
};

// Single NTP probe sample
typedef struct {
    int64_t theta_us;   // Clock offset (client - server) in microseconds
    int64_t rtt_us;     // Round-trip time in microseconds
    bool valid;         // Whether this sample is valid
} ntp_sample_t;

// Mutex for PLL access from multiple threads
static SemaphoreHandle_t pll_mutex = NULL;

/**
 * @brief Sends a single NTP query and returns offset + RTT
 *
 * @param server_ip NTP server IP address string
 * @param sample Output sample structure
 * @return true if successful, false otherwise
 */
static bool ntp_query_single(const char *server_ip, ntp_sample_t *sample) {
    if (!server_ip || !sample) return false;

    sample->valid = false;

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    // Set receive timeout
    struct timeval tv = { .tv_sec = 0, .tv_usec = NTP_PROBE_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Setup server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(NTP_SERVER_PORT);

    // Prepare NTP request packet (48 bytes)
    uint8_t ntp_packet[48];
    memset(ntp_packet, 0, sizeof(ntp_packet));
    ntp_packet[0] = 0x23; // LI=0, Version=4, Mode=3 (client)

    // Capture T1 (client transmit time)
    int64_t t1_us = esp_timer_get_time();

    // Send NTP request
    if (sendto(sock, ntp_packet, sizeof(ntp_packet), 0,
               (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return false;
    }

    // Receive NTP response
    uint8_t ntp_response[48];
    socklen_t socklen = sizeof(server_addr);
    int r = recvfrom(sock, ntp_response, sizeof(ntp_response), 0,
                     (struct sockaddr*)&server_addr, &socklen);

    // Capture T4 (client receive time)
    int64_t t4_us = esp_timer_get_time();

    close(sock);

    if (r != sizeof(ntp_response)) {
        return false;
    }

    // Extract server timestamps from NTP response
    // T2 = receive timestamp (bytes 32-39)
    // T3 = transmit timestamp (bytes 40-47)
    uint32_t t3_sec = ((uint32_t)ntp_response[40] << 24) |
                      ((uint32_t)ntp_response[41] << 16) |
                      ((uint32_t)ntp_response[42] << 8) |
                      (uint32_t)ntp_response[43];

    uint32_t t3_frac = ((uint32_t)ntp_response[44] << 24) |
                       ((uint32_t)ntp_response[45] << 16) |
                       ((uint32_t)ntp_response[46] << 8) |
                       (uint32_t)ntp_response[47];

    // Convert NTP time to Unix microseconds
    // NTP epoch is 1900-01-01, Unix epoch is 1970-01-01 (difference: 2208988800 seconds)
    int64_t t3_unix_us = ((int64_t)(t3_sec - 2208988800UL) * 1000000LL) +
                         ((int64_t)t3_frac * 1000000LL / 4294967296LL);

    // Calculate offset using NTP algorithm: theta = ((T2-T1) + (T3-T4)) / 2
    // We assume T2 ≈ T3 for simplicity (server processing time negligible)
    // So: theta ≈ T3 - (T1 + T4)/2
    sample->theta_us = t3_unix_us - (t1_us + t4_us) / 2;
    sample->rtt_us = t4_us - t1_us;
    sample->valid = true;

    return true;
}

/**
 * @brief Performs an NTP micro-probe burst and returns the min-RTT sample
 *
 * @param server_ip NTP server IP address string
 * @param best_sample Output: best (min-RTT) sample from the burst
 * @return true if at least one valid sample was obtained
 */
static bool ntp_micro_probe_burst(const char *server_ip, ntp_sample_t *best_sample) {
    if (!server_ip || !best_sample) return false;

    best_sample->valid = false;
    int64_t min_rtt = INT64_MAX;
    int valid_count = 0;

    for (int i = 0; i < NTP_PROBE_BURST_SIZE; i++) {
        ntp_sample_t sample;
        if (ntp_query_single(server_ip, &sample) && sample.valid) {
            valid_count++;

            // Keep the sample with minimum RTT
            if (sample.rtt_us < min_rtt) {
                min_rtt = sample.rtt_us;
                *best_sample = sample;
            }
        }

        // Small delay between probes (10ms)
        if (i < NTP_PROBE_BURST_SIZE - 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (valid_count > 0) {
        ESP_LOGI(TAG, "NTP micro-probe: %d/%d valid, best RTT=%" PRId64 " µs, offset=%+" PRId64 " µs",
                 valid_count, NTP_PROBE_BURST_SIZE, best_sample->rtt_us, best_sample->theta_us);
    }

    return best_sample->valid;
}

/**
 * @brief Updates the PLL with a new offset measurement
 *
 * @param sample NTP sample with offset and RTT
 */
static void pll_update(const ntp_sample_t *sample) {
    if (!sample || !sample->valid) return;

    xSemaphoreTake(pll_mutex, portMAX_DELAY);

    int64_t now_mono_us = esp_timer_get_time();
    int64_t master_us = now_mono_us + sample->theta_us;  // Master time = local + offset

    if (!pll.valid) {
        // First sample: just initialize
        pll.a = 1.0;
        pll.b = (double)sample->theta_us;
        pll.last_update_mono_us = now_mono_us;
        pll.last_master_us = master_us;
        pll.valid = true;
        pll.sample_count = 1;

        ESP_LOGI(TAG, "PLL initialized: offset=%.1f µs", pll.b);
    } else {
        // Subsequent samples: simple exponential smoothing for offset
        // For skew: track drift over time

        int64_t dt_mono = now_mono_us - pll.last_update_mono_us;
        int64_t dt_master = master_us - pll.last_master_us;

        if (dt_mono > 1000000) {  // At least 1 second between updates for skew calculation
            // Calculate observed skew
            double new_a = (double)dt_master / (double)dt_mono;

            // Smooth skew with time constant
            double alpha_skew = 0.1;  // Slow adaptation for skew
            pll.a = (1.0 - alpha_skew) * pll.a + alpha_skew * new_a;

            pll.last_update_mono_us = now_mono_us;
            pll.last_master_us = master_us;
        }

        // Smooth offset with faster time constant
        double alpha_offset = 0.3;
        pll.b = (1.0 - alpha_offset) * pll.b + alpha_offset * (double)sample->theta_us;

        pll.sample_count++;

        double skew_ppm = (pll.a - 1.0) * 1e6;
        ESP_LOGD(TAG, "PLL updated: offset=%.1f µs, skew=%.1f ppm, samples=%d",
                 pll.b, skew_ppm, pll.sample_count);
    }

    xSemaphoreGive(pll_mutex);
}

// SNTP sync notification callback
static void time_sync_notification_cb(struct timeval *tv) {
    struct tm timeinfo;
    char strftime_buf[64];

    localtime_r(&tv->tv_sec, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

    // Calculate time variance if we have a previous time sample
    if (last_known_time.tv_sec != 0) {
        // Get current tick count to calculate elapsed time since last sample
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - last_known_time_us;

        // Calculate what the current time should be based on last known time + elapsed
        int64_t expected_time_us = (int64_t)last_known_time.tv_sec * 1000000LL +
                                   (int64_t)last_known_time.tv_usec + elapsed_us;

        // New synced time in microseconds
        int64_t synced_time_us = (int64_t)tv->tv_sec * 1000000LL + (int64_t)tv->tv_usec;

        // Calculate the variance (how much the time jumped)
        int64_t variance_us = synced_time_us - expected_time_us;
        double variance_ms = (double)variance_us / 1000.0;

        // Log if variance is more than 1ms
        if (llabs(variance_us) > 1000) {  // 1000 us = 1 ms
            ESP_LOGW(TAG, "Time adjusted by %.3f ms (variance > 1ms threshold)", variance_ms);
            ESP_LOGI(TAG, "Time synchronized: %s.%06ld (adjustment: %+.3f ms)",
                     strftime_buf, tv->tv_usec, variance_ms);
        } else {
            ESP_LOGI(TAG, "Time synchronized: %s.%06ld (adjustment: %+.3f ms)",
                     strftime_buf, tv->tv_usec, variance_ms);
        }
    } else {
        ESP_LOGI(TAG, "Time synchronized (first sync): %s.%06ld", strftime_buf, tv->tv_usec);
    }

    // Update last known time for next comparison
    last_known_time = *tv;
    last_known_time_us = esp_timer_get_time();
}

// Function to query mDNS for screamrouter
static bool query_mdns_for_ntp_server(char *ip_address, size_t ip_address_size) {
    // Prepare hostname to query
    char hostname[64];
    snprintf(hostname, sizeof(hostname), "%s", QUERY_TARGET);

    // Query for A record (IPv4 address)
    esp_ip4_addr_t addr;
    memset(&addr, 0, sizeof(addr));  // Initialize to zero

    esp_err_t err = mdns_query_a(hostname, MDNS_QUERY_TIMEOUT_MS, &addr);

    if (err == ESP_OK) {
        // Safely convert IP to string
        if (ip_address && ip_address_size > 0) {
            // Use manual formatting for safety
            snprintf(ip_address, ip_address_size, "%d.%d.%d.%d",
                     IP2STR(&addr));
            ESP_LOGI(TAG, "mDNS query successful: %s.local -> %s", QUERY_TARGET, ip_address);
        }
        return true;
    }
    ESP_LOGW(TAG, "mDNS query failed for %s.local: %s", QUERY_TARGET, esp_err_to_name(err));
    return false;
}

// Function to update SNTP server
static void update_sntp_server(const char *server_ip) {
    if (!sntp_initialized) {
        // Initialize SNTP for the first time
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server_ip);
        config.start = false;  // Don't start automatically, we'll start manually
        config.sync_cb = time_sync_notification_cb;

        esp_err_t err = esp_netif_sntp_init(&config);
        if (err == ESP_OK) {
            esp_netif_sntp_start();
            sntp_initialized = true;
            ESP_LOGI(TAG, "SNTP initialized and started with server: %s", server_ip);
        } else {
            ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(err));
        }
    } else {
        // Update existing SNTP server
        esp_sntp_stop();
        esp_sntp_setservername(0, server_ip);
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP server updated to: %s", server_ip);
    }
}

static void ntp_client_task(void *pvParameters) {
    char ntp_server_address[46] = {0};
    TickType_t last_mdns_check = 0;
    TickType_t last_probe_time = 0;

    // Probe rate control (start with fast probes for initial lock)
    uint32_t probe_interval_ms = 200;  // Start with 200ms (5 Hz)
    const uint32_t initial_probe_count = 10;  // Number of fast initial probes
    const uint32_t steady_probe_interval_ms = 1000;  // 1 Hz steady-state
    uint32_t probe_count = 0;

    // Main Loop
    while (1) {
        bool ip_found = false;
        bool ip_changed = false;

        // Check if we have a valid DNS cache
        if (dns_cache.valid) {
            // Use cached IP address
            strncpy(ntp_server_address, dns_cache.ip_address, sizeof(ntp_server_address) - 1);
            ntp_server_address[sizeof(ntp_server_address) - 1] = '\0';
            ip_found = true;
        } else {
            // Check if it's time to query mDNS
            TickType_t now = xTaskGetTickCount();
            if ((now - last_mdns_check) >= pdMS_TO_TICKS(MDNS_CHECK_INTERVAL_MS)) {
                char new_ip[46] = {0};

                // Query mDNS for screamrouter
                if (query_mdns_for_ntp_server(new_ip, sizeof(new_ip))) {
                    // Check if IP changed
                    if (strcmp(dns_cache.ip_address, new_ip) != 0) {
                        ip_changed = true;
                        strncpy(ntp_server_address, new_ip, sizeof(ntp_server_address) - 1);
                        ntp_server_address[sizeof(ntp_server_address) - 1] = '\0';

                        // Update DNS cache
                        strncpy(dns_cache.ip_address, new_ip, sizeof(dns_cache.ip_address) - 1);
                        dns_cache.ip_address[sizeof(dns_cache.ip_address) - 1] = '\0';
                        dns_cache.valid = true;
                        dns_cache.failure_count = 0;
                        ip_found = true;

                        ESP_LOGI(TAG, "NTP server IP updated and cached: %s", ntp_server_address);

                        // Reset PLL on server change
                        xSemaphoreTake(pll_mutex, portMAX_DELAY);
                        pll.valid = false;
                        pll.sample_count = 0;
                        probe_count = 0;  // Restart fast probing
                        xSemaphoreGive(pll_mutex);
                    } else {
                        ip_found = true;
                    }
                } else {
                    // mDNS query failed
                    dns_cache.failure_count++;
                    ESP_LOGD(TAG, "mDNS query failure, failure count: %d/%d",
                             dns_cache.failure_count, MAX_FAILURE_COUNT);

                    // Invalidate DNS cache after too many consecutive failures
                    if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                        dns_cache.valid = false;
                        dns_cache.failure_count = 0;
                        ESP_LOGW(TAG, "Invalidated DNS cache due to %d consecutive mDNS failures",
                                 MAX_FAILURE_COUNT);
                    }
                }

                last_mdns_check = now;
            }
        }

        // Update SNTP server if IP was found and either changed or SNTP not initialized yet
        if (ip_found && (ip_changed || !sntp_initialized)) {
            update_sntp_server(ntp_server_address);
        }

        // Update last known time for variance tracking (only if SNTP is initialized)
        if (sntp_initialized) {
            gettimeofday(&last_known_time, NULL);
            last_known_time_us = esp_timer_get_time();
        }

        // NTP micro-probe for precision audio sync
        if (ip_found) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_probe_time) >= pdMS_TO_TICKS(probe_interval_ms)) {
                // Perform micro-probe burst
                ntp_sample_t sample;
                if (ntp_micro_probe_burst(ntp_server_address, &sample)) {
                    pll_update(&sample);
                    probe_count++;

                    // After initial fast probing, slow down to steady-state rate
                    if (probe_count >= initial_probe_count && probe_interval_ms < steady_probe_interval_ms) {
                        probe_interval_ms = steady_probe_interval_ms;
                        ESP_LOGI(TAG, "Switching to steady-state probe rate (%u ms)", probe_interval_ms);
                    }
                }

                last_probe_time = now;
            }
        }

        // Sleep for a short time to allow other tasks to run
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Cleanup (should not be reached in normal operation)
    vTaskDelete(NULL);
}

// ============================================================================
// PUBLIC API for audio subsystem
// ============================================================================

/**
 * @brief Convert master (NTP) time to local monotonic time
 *
 * @param master_us Master time in Unix microseconds
 * @return Local monotonic time in microseconds, or -1 if PLL not valid
 */
extern "C" int64_t ntp_master_to_local(int64_t master_us) {
    if (!pll_mutex) return -1;

    xSemaphoreTake(pll_mutex, portMAX_DELAY);

    int64_t local_us = -1;
    if (pll.valid && pll.a != 0.0) {
        // local_mono = (master_us - b) / a
        local_us = (int64_t)((double)(master_us) - pll.b) / pll.a;
    }

    xSemaphoreGive(pll_mutex);
    return local_us;
}

/**
 * @brief Convert local monotonic time to master (NTP) time
 *
 * @param local_mono_us Local monotonic time in microseconds
 * @return Master time in Unix microseconds, or -1 if PLL not valid
 */
extern "C" int64_t ntp_local_to_master(int64_t local_mono_us) {
    if (!pll_mutex) return -1;

    xSemaphoreTake(pll_mutex, portMAX_DELAY);

    int64_t master_us = -1;
    if (pll.valid) {
        // master_us = a * local_mono + b
        master_us = (int64_t)(pll.a * (double)local_mono_us + pll.b);
    }

    xSemaphoreGive(pll_mutex);
    return master_us;
}

/**
 * @brief Get current PLL offset and skew
 *
 * @param offset_us Output: current offset in microseconds (can be NULL)
 * @param skew_ppm Output: current skew in ppm (can be NULL)
 * @return true if PLL is valid, false otherwise
 */
extern "C" bool ntp_get_pll_state(double *offset_us, double *skew_ppm) {
    if (!pll_mutex) return false;

    xSemaphoreTake(pll_mutex, portMAX_DELAY);

    bool valid = pll.valid;
    if (valid) {
        if (offset_us) *offset_us = pll.b;
        if (skew_ppm) *skew_ppm = (pll.a - 1.0) * 1e6;
    }

    xSemaphoreGive(pll_mutex);
    return valid;
}

/**
 * @brief Manually trigger an NTP micro-probe burst (for testing or immediate sync)
 *
 * @return true if successful, false otherwise
 */
extern "C" bool ntp_trigger_probe() {
    if (!dns_cache.valid) {
        ESP_LOGW(TAG, "Cannot trigger probe: no NTP server IP cached");
        return false;
    }

    ntp_sample_t sample;
    if (ntp_micro_probe_burst(dns_cache.ip_address, &sample)) {
        pll_update(&sample);
        return true;
    }

    return false;
}

extern "C" void initialize_ntp_client() {
    // Prevent multiple initializations
    if (ntp_client_initialized) {
        ESP_LOGW(TAG, "NTP client already initialized");
        return;
    }

    // Create PLL mutex
    if (!pll_mutex) {
        pll_mutex = xSemaphoreCreateMutex();
        if (!pll_mutex) {
            ESP_LOGE(TAG, "Failed to create PLL mutex");
            return;
        }
    }

    // Create the NTP client task
    BaseType_t ret = xTaskCreatePinnedToCore(
        ntp_client_task,
        "ntp_client_task",
        8192,  // Increased stack size for micro-probes
        NULL,
        23,
        &ntp_client_task_handle,
        0
    );

    if (ret == pdPASS) {
        ntp_client_initialized = true;
        ESP_LOGI(TAG, "NTP client task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create NTP client task");
    }
}

extern "C" void deinitialize_ntp_client() {
    if (!ntp_client_initialized || ntp_client_task_handle == NULL) {
        ESP_LOGW(TAG, "NTP client not initialized or already deinitialized");
        return;
    }

    // Stop SNTP if it was initialized
    if (sntp_initialized) {
        esp_netif_sntp_deinit();
        sntp_initialized = false;
        ESP_LOGI(TAG, "SNTP deinitialized");
    }

    // Delete the task
    vTaskDelete(ntp_client_task_handle);
    ntp_client_task_handle = NULL;
    ntp_client_initialized = false;

    // Clear DNS cache
    dns_cache.valid = false;
    dns_cache.failure_count = 0;
    memset(dns_cache.ip_address, 0, sizeof(dns_cache.ip_address));

    ESP_LOGI(TAG, "NTP client deinitialized");
}
