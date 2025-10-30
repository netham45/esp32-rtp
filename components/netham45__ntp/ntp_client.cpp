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
#include "lwip/netdb.h"
#include "lwip/inet.h"
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

// Clock correction thresholds
#define SLEW_THRESHOLD_US      100000LL   // 100ms - below this, use slewing
#define STEP_THRESHOLD_US    10000000LL   // 10s - above this, use stepping with warning
#define MIN_CORRECTION_US         500LL   // 500µs - minimum correction to apply
#define MAX_SLEW_RATE_US        10000LL   // 10ms max correction per iteration

// Task handle for NTP client
static TaskHandle_t ntp_client_task_handle = NULL;
static bool ntp_client_initialized = false;
static bool sntp_initialized = false;

// Runtime configuration
static bool s_use_mdns = true;            // true: resolve screamrouter via mDNS
static char s_custom_host[64] = {0};      // custom server hostname/IP when not using mDNS
static uint16_t s_custom_port = 123;      // micro-probe UDP port (SNTP always uses 123)

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
    int64_t total_correction_us;  // Total corrections applied
    int correction_count;         // Number of corrections
    int64_t last_error_us;       // Last error magnitude for convergence tracking
} ntp_pll_t;

static ntp_pll_t pll = {
    .a = 1.0,
    .b = 0.0,
    .last_update_mono_us = 0,
    .last_master_us = 0,
    .valid = false,
    .sample_count = 0,
    .total_correction_us = 0,
    .correction_count = 0,
    .last_error_us = 0
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
static bool ntp_query_single(const char *server_ip, ntp_sample_t *sample, uint16_t port) {
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
    server_addr.sin_port = htons(port);

    // Prepare NTP request packet (48 bytes)
    uint8_t ntp_packet[48];
    memset(ntp_packet, 0, sizeof(ntp_packet));
    ntp_packet[0] = 0x23; // LI=0, Version=4, Mode=3 (client)

    // Capture T1 (client transmit time)
    struct timeval t1_tv;
    gettimeofday(&t1_tv, NULL);
    int64_t t1_us = (int64_t)t1_tv.tv_sec * 1000000LL + (int64_t)t1_tv.tv_usec;
    
    // If system time is not initialized yet (year < 2020), use monotonic time
    bool use_monotonic = (t1_tv.tv_sec < 1577836800); // Jan 1, 2020
    if (use_monotonic) {
        t1_us = esp_timer_get_time();
    }

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
    int64_t t4_us;
    if (use_monotonic) {
        t4_us = esp_timer_get_time();
    } else {
        struct timeval t4_tv;
        gettimeofday(&t4_tv, NULL);
        t4_us = (int64_t)t4_tv.tv_sec * 1000000LL + (int64_t)t4_tv.tv_usec;
    }

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
    if (use_monotonic) {
        // When using monotonic time, we need to calculate offset differently
        // We can't directly compare monotonic time to Unix time
        // Instead, set the offset to the full Unix time minus half RTT
        int64_t rtt_us = t4_us - t1_us;
        int64_t midpoint_us = t1_us + rtt_us / 2;
        
        // Get current system time to calculate how to adjust
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        int64_t now_unix_us = (int64_t)now_tv.tv_sec * 1000000LL + (int64_t)now_tv.tv_usec;
        int64_t now_mono_us = esp_timer_get_time();
        
        // The server time at midpoint was t3_unix_us
        // Our monotonic time at midpoint was midpoint_us
        // So offset = server_time - (system_time - mono_time + midpoint_mono)
        sample->theta_us = t3_unix_us - now_unix_us + (now_mono_us - midpoint_us);
        sample->rtt_us = rtt_us;
    } else {
        sample->theta_us = t3_unix_us - (t1_us + t4_us) / 2;
        sample->rtt_us = t4_us - t1_us;
    }
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
static bool ntp_micro_probe_burst(const char *server_ip, uint16_t port, ntp_sample_t *best_sample) {
    if (!server_ip || !best_sample) return false;

    best_sample->valid = false;
    int64_t min_rtt = INT64_MAX;
    int valid_count = 0;

    for (int i = 0; i < NTP_PROBE_BURST_SIZE; i++) {
        ntp_sample_t sample;
        if (ntp_query_single(server_ip, &sample, port) && sample.valid) {
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

    // Check if system time is initialized
    struct timeval now_tv;
    gettimeofday(&now_tv, NULL);
    bool system_time_valid = (now_tv.tv_sec >= 1577836800); // Jan 1, 2020
    
    // If system time not valid and we have a large offset, set it first
    if (!system_time_valid && llabs(sample->theta_us) > 1000000LL) {  // > 1 second
        int64_t now_mono_us = esp_timer_get_time();
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t current_unix_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
        int64_t corrected_unix_us = current_unix_us + sample->theta_us;
        
        tv.tv_sec = corrected_unix_us / 1000000LL;
        tv.tv_usec = corrected_unix_us % 1000000LL;
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "System clock initialized from NTP: %ld.%06ld", tv.tv_sec, tv.tv_usec);
        
        // After setting system time, recalculate using the new system time
        gettimeofday(&now_tv, NULL);
    }

    int64_t now_mono_us = esp_timer_get_time();
    int64_t now_unix_us = (int64_t)now_tv.tv_sec * 1000000LL + (int64_t)now_tv.tv_usec;
    
    // For PLL, we track the relationship between monotonic time and Unix time
    // This allows us to maintain precision even if system time jumps
    
    if (!pll.valid) {
        // First sample: initialize PLL
        // The offset represents: unix_time = mono_time + offset
        pll.a = 1.0;  // No skew initially
        pll.b = (double)(now_unix_us - now_mono_us);
        pll.last_update_mono_us = now_mono_us;
        pll.last_master_us = now_unix_us;
        pll.valid = true;
        pll.sample_count = 1;

        ESP_LOGI(TAG, "PLL initialized: offset=%.1f µs (system-mono delta)", pll.b);
    } else {
        // Subsequent samples: update PLL based on new measurement
        // The sample offset tells us how much to adjust our current time estimate
        
        int64_t expected_unix_us = (int64_t)(pll.a * (double)now_mono_us + pll.b);
        int64_t actual_unix_us = now_unix_us + sample->theta_us;
        int64_t error_us = actual_unix_us - expected_unix_us;
        
        // Apply clock corrections based on the raw NTP offset BEFORE updating PLL state
        // Use sample->theta_us for corrections, not the PLL error
        bool correction_applied = false;
        int64_t correction_us = 0;
        int64_t offset_to_correct = sample->theta_us;  // Use raw NTP offset for corrections
        
        if (llabs(offset_to_correct) > MIN_CORRECTION_US) {
            if (llabs(offset_to_correct) < SLEW_THRESHOLD_US) {
                // SLEWING: Apply gradual correction for small offsets
                // Be more aggressive for persistent offsets
                correction_us = offset_to_correct;
                
                // Increase correction rate if offset persists
                int64_t max_correction = MAX_SLEW_RATE_US;
                if (pll.correction_count > 5 && llabs(offset_to_correct) > 2000) {
                    // Double the correction rate for persistent offsets > 2ms
                    max_correction = MAX_SLEW_RATE_US * 2;
                }
                
                if (llabs(correction_us) > max_correction) {
                    correction_us = (error_us > 0) ? max_correction : -max_correction;
                }
                
                // Apply partial correction
                struct timeval tv;
                gettimeofday(&tv, NULL);
                int64_t current_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
                int64_t corrected_us = current_us + correction_us;
                
                tv.tv_sec = corrected_us / 1000000LL;
                tv.tv_usec = corrected_us % 1000000LL;
                settimeofday(&tv, NULL);
                
                pll.total_correction_us += correction_us;
                pll.correction_count++;
                correction_applied = true;
                
                ESP_LOGD(TAG, "Clock slew: applied %+" PRId64 " µs correction (offset was %+" PRId64 " µs, total corrections: %+" PRId64 " µs)",
                         correction_us, offset_to_correct, pll.total_correction_us);
                
                // Log convergence progress periodically
                if (pll.correction_count % 10 == 0) {
                    ESP_LOGI(TAG, "Clock slewing in progress: current offset=%+.3f ms, corrections applied=%d, total correction=%+.3f ms",
                             (double)offset_to_correct / 1000.0, pll.correction_count, (double)pll.total_correction_us / 1000.0);
                }
                
            } else if (llabs(offset_to_correct) < STEP_THRESHOLD_US) {
                // STEPPING: Immediate correction for medium offsets
                struct timeval tv;
                tv.tv_sec = actual_unix_us / 1000000LL;
                tv.tv_usec = actual_unix_us % 1000000LL;
                settimeofday(&tv, NULL);
                
                correction_us = error_us;
                pll.total_correction_us += error_us;
                pll.correction_count++;
                correction_applied = true;
                
                ESP_LOGI(TAG, "Clock step: corrected %+.3f ms offset (total corrections: %+" PRId64 " µs)",
                         (double)offset_to_correct / 1000.0, pll.total_correction_us);
                
            } else {
                // Large offset - step with warning
                ESP_LOGW(TAG, "Large time offset detected: %.3f seconds", (double)offset_to_correct / 1e6);
                
                struct timeval tv;
                tv.tv_sec = actual_unix_us / 1000000LL;
                tv.tv_usec = actual_unix_us % 1000000LL;
                settimeofday(&tv, NULL);
                
                correction_us = error_us;
                pll.total_correction_us += error_us;
                pll.correction_count++;
                correction_applied = true;
                
                ESP_LOGI(TAG, "System clock adjusted from NTP: %ld.%06ld (large step)", tv.tv_sec, tv.tv_usec);
            }
            
            // Don't reduce PLL error - let it track naturally
        } else {
            // Offset below minimum threshold - log convergence if we were correcting
            if (pll.correction_count > 0 && llabs(pll.last_error_us) > MIN_CORRECTION_US) {
                ESP_LOGI(TAG, "Clock converged: offset now within %+" PRId64 " µs (below %lld µs threshold)",
                         offset_to_correct, MIN_CORRECTION_US);
            }
        }
        
        // Update offset with exponential smoothing using the original PLL error
        double alpha_offset = 0.3;
        pll.b += alpha_offset * (double)error_us;  // Use original PLL error for tracking
        
        // Update skew if enough time has passed
        int64_t dt_mono = now_mono_us - pll.last_update_mono_us;
        if (dt_mono > 1000000) {  // At least 1 second
            int64_t dt_master = actual_unix_us - pll.last_master_us;
            double new_a = (double)dt_master / (double)dt_mono;
            
            double alpha_skew = 0.1;
            pll.a = (1.0 - alpha_skew) * pll.a + alpha_skew * new_a;
            
            pll.last_update_mono_us = now_mono_us;
            pll.last_master_us = actual_unix_us;
        }
        
        pll.sample_count++;
        pll.last_error_us = error_us;  // Store the adjusted error after correction
        
        double skew_ppm = (pll.a - 1.0) * 1e6;
        ESP_LOGD(TAG, "PLL updated: offset=%.1f µs, skew=%.1f ppm, error=%.1f µs (after correction), samples=%d",
                 pll.b, skew_ppm, (double)error_us, pll.sample_count);
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

                if (s_use_mdns) {
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

                            ESP_LOGI(TAG, "NTP server IP updated and cached (mDNS): %s", ntp_server_address);

                            // Reset PLL on server change
                            xSemaphoreTake(pll_mutex, portMAX_DELAY);
                            pll.valid = false;
                            pll.sample_count = 0;
                            pll.total_correction_us = 0;
                            pll.correction_count = 0;
                            pll.last_error_us = 0;
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
                } else {
                    // DNS resolve custom host to IPv4
                    struct hostent* he = NULL;
                    if (s_custom_host[0] != '\0') {
                        he = gethostbyname(s_custom_host);
                    }
                    if (he && he->h_addr_list && he->h_addr_list[0]) {
                        struct in_addr addr;
                        memcpy(&addr, he->h_addr_list[0], sizeof(addr));
                        const char* ipstr = inet_ntoa(addr);
                        if (ipstr) {
                            snprintf(new_ip, sizeof(new_ip), "%s", ipstr);
                            if (strcmp(dns_cache.ip_address, new_ip) != 0) {
                                ip_changed = true;
                                strncpy(ntp_server_address, new_ip, sizeof(ntp_server_address) - 1);
                                ntp_server_address[sizeof(ntp_server_address) - 1] = '\0';

                                strncpy(dns_cache.ip_address, new_ip, sizeof(dns_cache.ip_address) - 1);
                                dns_cache.ip_address[sizeof(dns_cache.ip_address) - 1] = '\0';
                                dns_cache.valid = true;
                                dns_cache.failure_count = 0;
                                ip_found = true;

                                ESP_LOGI(TAG, "NTP server IP updated and cached (DNS): %s (%s:%u)",
                                         ntp_server_address, s_custom_host, (unsigned)s_custom_port);

                                // Reset PLL on server change
                                xSemaphoreTake(pll_mutex, portMAX_DELAY);
                                pll.valid = false;
                                pll.sample_count = 0;
                                pll.total_correction_us = 0;
                                pll.correction_count = 0;
                                pll.last_error_us = 0;
                                probe_count = 0;  // Restart fast probing
                                xSemaphoreGive(pll_mutex);
                            } else {
                                ip_found = true;
                            }
                        }
                    } else {
                        // DNS resolution failed
                        dns_cache.failure_count++;
                        ESP_LOGD(TAG, "DNS resolution failure for '%s', failure count: %d/%d",
                                 s_custom_host, dns_cache.failure_count, MAX_FAILURE_COUNT);

                        if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                            dns_cache.valid = false;
                            dns_cache.failure_count = 0;
                            ESP_LOGW(TAG, "Invalidated DNS cache due to %d consecutive DNS failures",
                                     MAX_FAILURE_COUNT);
                        }
                    }
                }

                last_mdns_check = now;
            }
        }

        // Update SNTP server if IP was found and either changed or SNTP not initialized yet
        if (ip_found && (ip_changed || !sntp_initialized)) {
            const char* sntp_target = s_use_mdns ? ntp_server_address : s_custom_host;
            update_sntp_server(sntp_target);
        }

        // Update last known time for variance tracking (only if SNTP is initialized)
        if (sntp_initialized) {
            gettimeofday(&last_known_time, NULL);
            last_known_time_us = esp_timer_get_time();

            // Log SNTP sync status periodically
            static TickType_t last_status_log = 0;
            TickType_t now = xTaskGetTickCount();
            if ((now - last_status_log) >= pdMS_TO_TICKS(10000)) {  // Every 10 seconds
                sntp_sync_status_t status = esp_sntp_get_sync_status();
                const char* status_str = "UNKNOWN";
                switch (status) {
                    case SNTP_SYNC_STATUS_RESET: status_str = "RESET"; break;
                    case SNTP_SYNC_STATUS_COMPLETED: status_str = "COMPLETED"; break;
                    case SNTP_SYNC_STATUS_IN_PROGRESS: status_str = "IN_PROGRESS"; break;
                    default: break;
                }
                ESP_LOGI(TAG, "SNTP status: %s, System time: %ld.%06ld",
                         status_str, last_known_time.tv_sec, last_known_time.tv_usec);
                last_status_log = now;
            }
        }

        // NTP micro-probe for precision audio sync
        if (ip_found) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_probe_time) >= pdMS_TO_TICKS(probe_interval_ms)) {
                // Perform micro-probe burst
                ntp_sample_t sample;
                uint16_t probe_port = s_use_mdns ? 123 : s_custom_port;
                if (ntp_micro_probe_burst(ntp_server_address, probe_port, &sample)) {
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
 * @brief Get current PLL convergence status
 *
 * @param corrections_applied Output: total number of corrections applied (can be NULL)
 * @param total_correction_us Output: total correction amount in microseconds (can be NULL)
 * @param current_error_us Output: current error in microseconds (can be NULL)
 * @return true if PLL is valid and converged (error < MIN_CORRECTION_US), false otherwise
 */
extern "C" bool ntp_get_convergence_status(int *corrections_applied, int64_t *total_correction_us, int64_t *current_error_us) {
    if (!pll_mutex) return false;
    
    xSemaphoreTake(pll_mutex, portMAX_DELAY);
    
    bool converged = false;
    if (pll.valid) {
        if (corrections_applied) *corrections_applied = pll.correction_count;
        if (total_correction_us) *total_correction_us = pll.total_correction_us;
        if (current_error_us) *current_error_us = pll.last_error_us;
        
        converged = (llabs(pll.last_error_us) < MIN_CORRECTION_US);
    }
    
    xSemaphoreGive(pll_mutex);
    return converged;
}

/**
 * @brief Manually trigger an NTP micro-probe burst (for testing or immediate sync)
 *
 * @return true if successful, false otherwise
 */
extern "C" void ntp_client_set_config(bool use_mdns, const char* host, uint16_t port) {
    s_use_mdns = use_mdns;
    if (host) {
        strncpy(s_custom_host, host, sizeof(s_custom_host) - 1);
        s_custom_host[sizeof(s_custom_host) - 1] = '\0';
    }
    s_custom_port = (port == 0) ? 123 : port;

    // Invalidate cache so task will resolve next cycle
    dns_cache.valid = false;
    dns_cache.failure_count = 0;
    memset(dns_cache.ip_address, 0, sizeof(dns_cache.ip_address));

    // Update SNTP target immediately if already initialized
    if (sntp_initialized) {
        if (s_use_mdns) {
            // Will be set on next mDNS discovery
        } else {
            update_sntp_server(s_custom_host);
        }
    }

    ESP_LOGI(TAG, "NTP config set: use_mdns=%d, host=%s, port=%u",
             (int)s_use_mdns, s_custom_host[0] ? s_custom_host : "(none)", (unsigned)s_custom_port);
}

extern "C" bool ntp_trigger_probe() {
    if (!dns_cache.valid) {
        ESP_LOGW(TAG, "Cannot trigger probe: no NTP server IP cached");
        return false;
    }

    ntp_sample_t sample;
    uint16_t probe_port = s_use_mdns ? 123 : s_custom_port;
    if (ntp_micro_probe_burst(dns_cache.ip_address, probe_port, &sample)) {
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
