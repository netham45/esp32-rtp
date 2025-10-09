#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h> // Required for errno
#include <inttypes.h> // For PRI macros
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "ntp_client.h" // Uses extern "C" linkage from the header
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "time.h"
#include <algorithm> // For std::sort
#include <cmath>     // For sqrt
#include "mdns.h"    // ESP-IDF mDNS service

// IP address formatting macro
#ifndef IP2STR
#define IP2STR(addr) ((uint8_t*)&(addr))[0], ((uint8_t*)&(addr))[1], ((uint8_t*)&(addr))[2], ((uint8_t*)&(addr))[3]
#endif

#define NTP_SERVER_PORT 123
#define QUERY_TARGET "screamrouter"  // Will query for screamrouter.local
#define NTP_HISTORY_SIZE 25 // Store last 25 results
#define NTP_POLL_INTERVAL_MS 500 // Poll every .5 seconds
#define NTP_FAST_POLL_INTERVAL_MS 50 // Poll every 0.05 seconds when building initial samples
#define MAX_FAILURE_COUNT 3 // Maximum number of consecutive failures before invalidating the cache
#define MDNS_CHECK_INTERVAL_MS 5000 // Check mDNS every 5 seconds if no cached IP
#define MDNS_QUERY_TIMEOUT_MS 3000 // mDNS query timeout

static const char *TAG = "ntp_client";

// Task handle for NTP client
static TaskHandle_t ntp_client_task_handle = NULL;
static bool ntp_client_initialized = false;

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

// Buffer to store the last NTP_HISTORY_SIZE time results with microsecond precision
typedef struct {
    time_t timestamps[NTP_HISTORY_SIZE];
    int32_t microseconds[NTP_HISTORY_SIZE]; // Microsecond part of each timestamp
    int32_t round_trip_ms[NTP_HISTORY_SIZE]; // Round trip time in milliseconds
    int count;
    int index;
} ntp_history_t;

static ntp_history_t ntp_history = {
    .timestamps = {0},
    .microseconds = {0},
    .round_trip_ms = {0},
    .count = 0,
    .index = 0
};

// Function to add a new timestamp to the history
static void add_timestamp_to_history(time_t timestamp, int32_t microseconds, int32_t round_trip_ms) {
    ntp_history.timestamps[ntp_history.index] = timestamp;
    ntp_history.microseconds[ntp_history.index] = microseconds;
    ntp_history.round_trip_ms[ntp_history.index] = round_trip_ms;
    ntp_history.index = (ntp_history.index + 1) % NTP_HISTORY_SIZE;
    if (ntp_history.count < NTP_HISTORY_SIZE) {
        ntp_history.count++;
    }
}

// Function to calculate the median timestamp from history
static time_t calculate_median_timestamp() {
    if (ntp_history.count == 0) {
        return 0;
    }
    
    // Copy timestamps to a temporary array for sorting
    time_t temp[NTP_HISTORY_SIZE];
    memcpy(temp, ntp_history.timestamps, ntp_history.count * sizeof(time_t));
    
    // Sort the array
    std::sort(temp, temp + ntp_history.count);
    
    // Return the median value
    if (ntp_history.count % 2 == 0) {
        // Even number of elements, average the middle two
        return (temp[ntp_history.count/2 - 1] + temp[ntp_history.count/2]) / 2;
    } else {
        // Odd number of elements, return the middle one
        return temp[ntp_history.count/2];
    }
}

// Function to calculate jitter (standard deviation) of timestamps with microsecond precision
static double calculate_time_jitter(time_t *timestamps, int32_t *microseconds, int count) {
    if (count <= 1) {
        return 0.0;
    }
    
    // Calculate mean with microsecond precision
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += (double)timestamps[i] + (double)microseconds[i] / 1000000.0;
    }
    double mean = sum / count;
    
    // Calculate sum of squared differences
    double sum_squared_diff = 0.0;
    for (int i = 0; i < count; i++) {
        double timestamp_with_us = (double)timestamps[i] + (double)microseconds[i] / 1000000.0;
        double diff = timestamp_with_us - mean;
        sum_squared_diff += diff * diff;
    }
    
    // Calculate standard deviation (jitter)
    return sqrt(sum_squared_diff / count);
}

// Function to calculate network jitter (standard deviation of round trip times)
static double calculate_network_jitter(int32_t *round_trip_ms, int count) {
    if (count <= 1) {
        return 0.0;
    }
    
    // Calculate mean
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += (double)round_trip_ms[i];
    }
    double mean = sum / count;
    
    // Calculate sum of squared differences
    double sum_squared_diff = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = (double)round_trip_ms[i] - mean;
        sum_squared_diff += diff * diff;
    }
    
    // Calculate standard deviation (jitter)
    return sqrt(sum_squared_diff / count);
}

// Function to calculate min, max, and range of timestamps
static void calculate_time_range(time_t *timestamps, int count, time_t *min, time_t *max) {
    if (count <= 0) {
        *min = 0;
        *max = 0;
        return;
    }
    
    *min = timestamps[0];
    *max = timestamps[0];
    
    for (int i = 1; i < count; i++) {
        if (timestamps[i] < *min) *min = timestamps[i];
        if (timestamps[i] > *max) *max = timestamps[i];
    }
}

// Function to calculate median microseconds
static int32_t calculate_median_microseconds() {
    if (ntp_history.count == 0) {
        return 0;
    }
    
    // Create a temporary array of indices
    int indices[NTP_HISTORY_SIZE];
    for (int i = 0; i < ntp_history.count; i++) {
        indices[i] = i;
    }
    
    // Sort indices based on timestamps
    for (int i = 0; i < ntp_history.count - 1; i++) {
        for (int j = 0; j < ntp_history.count - i - 1; j++) {
            if (ntp_history.timestamps[indices[j]] > ntp_history.timestamps[indices[j + 1]] ||
                (ntp_history.timestamps[indices[j]] == ntp_history.timestamps[indices[j + 1]] && 
                 ntp_history.microseconds[indices[j]] > ntp_history.microseconds[indices[j + 1]])) {
                // Swap indices
                int temp = indices[j];
                indices[j] = indices[j + 1];
                indices[j + 1] = temp;
            }
        }
    }
    
    // Return the median microseconds
    if (ntp_history.count % 2 == 0) {
        // Even number of elements, average the middle two
        return (ntp_history.microseconds[indices[ntp_history.count/2 - 1]] + 
                ntp_history.microseconds[indices[ntp_history.count/2]]) / 2;
    } else {
        // Odd number of elements, return the middle one
        return ntp_history.microseconds[indices[ntp_history.count/2]];
    }
}

// Function to calculate median round trip time with outlier filtering
static int32_t calculate_median_round_trip() {
    if (ntp_history.count == 0) {
        return 0;
    }
    
    // Copy round trip times to a temporary array for sorting
    int32_t temp[NTP_HISTORY_SIZE];
    memcpy(temp, ntp_history.round_trip_ms, ntp_history.count * sizeof(int32_t));
    
    // Sort the array
    std::sort(temp, temp + ntp_history.count);
    
    // Calculate quartiles for outlier detection
    int q1_idx = ntp_history.count / 4;
    int q3_idx = (3 * ntp_history.count) / 4;
    int32_t q1 = temp[q1_idx];
    int32_t q3 = temp[q3_idx];
    int32_t iqr = q3 - q1;
    
    // Define bounds for outliers
    int32_t lower_bound = q1 - 1.5 * iqr;
    int32_t upper_bound = q3 + 1.5 * iqr;
    
    // Filter outliers
    int32_t filtered[NTP_HISTORY_SIZE];
    int filtered_count = 0;
    
    for (int i = 0; i < ntp_history.count; i++) {
        if (ntp_history.round_trip_ms[i] >= lower_bound && ntp_history.round_trip_ms[i] <= upper_bound) {
            filtered[filtered_count++] = ntp_history.round_trip_ms[i];
        }
    }
    
    // If we filtered out too many, fall back to original
    if (filtered_count < 3 && ntp_history.count >= 3) {
        //ESP_LOGW(TAG, "Too many RTT outliers filtered (%d/%d), using original data", 
                 //ntp_history.count - filtered_count, ntp_history.count);
        filtered_count = ntp_history.count;
        memcpy(filtered, temp, ntp_history.count * sizeof(int32_t));
    } else if (filtered_count < ntp_history.count) {
        //ESP_LOGD(TAG, "Filtered %d/%d RTT outliers (bounds: [%" PRId32 ", %" PRId32 "] ms)", 
                 //ntp_history.count - filtered_count, ntp_history.count, lower_bound, upper_bound);
    }
    
    // Sort the filtered array
    std::sort(filtered, filtered + filtered_count);
    
    // Return the median value
    if (filtered_count % 2 == 0) {
        // Even number of elements, average the middle two
        return (filtered[filtered_count/2 - 1] + filtered[filtered_count/2]) / 2;
    } else {
        // Odd number of elements, return the middle one
        return filtered[filtered_count/2];
    }
}

static void set_system_time(time_t time_value, int32_t microseconds, int32_t round_trip_ms) {
    // Add the new timestamp to our history
    add_timestamp_to_history(time_value, microseconds, round_trip_ms);
    
    // Only update system time if we have enough samples for network jitter calculation
    if (ntp_history.count >= 3) {
        // Calculate network jitter and median round trip time with outlier filtering
        double network_jitter = calculate_network_jitter(ntp_history.round_trip_ms, ntp_history.count);
        int32_t median_round_trip = calculate_median_round_trip();
        
        // Calculate time jitter and range (for logging only)
        double time_jitter = calculate_time_jitter(ntp_history.timestamps, ntp_history.microseconds, ntp_history.count);
        time_t min_time, max_time;
        calculate_time_range(ntp_history.timestamps, ntp_history.count, &min_time, &max_time);
        time_t range = max_time - min_time;
        
        // Use the most recent timestamp (not median) since we only care about network jitter
        time_t current_time = time_value;
        int32_t current_microseconds = microseconds;
        
        // Adjust time for network delay (half of median round trip time)
        int32_t one_way_delay_us = (median_round_trip * 1000) / 2; // Convert ms to us and divide by 2
        
        // Adjust microseconds for one-way delay
        int32_t adjusted_microseconds = current_microseconds + one_way_delay_us;
        
        // Handle overflow of microseconds
        time_t adjusted_time = current_time;
        if (adjusted_microseconds >= 1000000) {
            adjusted_time += adjusted_microseconds / 1000000;
            adjusted_microseconds %= 1000000;
        }
        
        struct timeval now = { .tv_sec = adjusted_time, .tv_usec = adjusted_microseconds };
        int ret = settimeofday(&now, NULL);
        if (ret == 0) {
            ESP_LOGD(TAG, "System time set successfully: %lld.%06" PRId32 " (using latest sample, adjusted for network delay)",
                     (long long)adjusted_time, adjusted_microseconds);
            
            // Verify the time was actually set
            struct timeval verify;
            gettimeofday(&verify, NULL);
            ESP_LOGD(TAG, "System time verification: %ld.%06ld (expected: %lld.%06" PRId32 ")",
                     (long)verify.tv_sec, (long)verify.tv_usec,
                     (long long)adjusted_time, adjusted_microseconds);
        } else {
            ESP_LOGE(TAG, "Failed to set system time: errno %d (%s)", errno, strerror(errno));
        }
        ESP_LOGD(TAG, "Time jitter: %.6f seconds, range: %lld seconds (min: %lld, max: %lld)",
                 time_jitter, (long long)range, (long long)min_time, (long long)max_time);
        ESP_LOGD(TAG, "Network stats: median RTT: %" PRId32 " ms, one-way delay: %" PRId32 " us, network jitter: %.3f ms",
                 median_round_trip, one_way_delay_us, network_jitter);
    } else {
        ESP_LOGD(TAG, "Added timestamp to history (%d/%" PRId32 " samples needed for jitter calculation)",
                 ntp_history.count, (int32_t)NTP_HISTORY_SIZE);
    }
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
            ESP_LOGD(TAG, "mDNS query successful: %s.local -> %s", QUERY_TARGET, ip_address);
        }
        return true;
    }
    ESP_LOGW(TAG, "mDNS query failed for %s.local: %s", QUERY_TARGET, esp_err_to_name(err));
    return false;
}

static void ntp_client_task(void *pvParameters) {
    char ntp_server_address[46] = {0};
    
    // Flag to track if we've completed initial sampling
    bool initial_sampling_complete = false;
    
    // Timing for mDNS checks
    TickType_t last_mdns_check = 0;

    // --- Main Loop ---
    while (1) {
        bool ip_found = false;

        // Check if we have a valid DNS cache
        if (dns_cache.valid) {
            // Use cached IP address - ensure null termination
            strncpy(ntp_server_address, dns_cache.ip_address, sizeof(ntp_server_address) - 1);
            ntp_server_address[sizeof(ntp_server_address) - 1] = '\0';
            ip_found = true;
        } else {
            // Check if it's time to query mDNS
            TickType_t now = xTaskGetTickCount();
            if ((now - last_mdns_check) >= pdMS_TO_TICKS(MDNS_CHECK_INTERVAL_MS)) {
                // Query mDNS for screamrouter
                if (query_mdns_for_ntp_server(ntp_server_address, sizeof(ntp_server_address))) {
                    // Update DNS cache - ensure null termination
                    strncpy(dns_cache.ip_address, ntp_server_address, sizeof(dns_cache.ip_address) - 1);
                    dns_cache.ip_address[sizeof(dns_cache.ip_address) - 1] = '\0';
                    dns_cache.valid = true;
                    dns_cache.failure_count = 0;
                    ip_found = true;
                    ESP_LOGD(TAG, "NTP server resolved and cached: %s", ntp_server_address);
                }
                
                last_mdns_check = now;
            }
        }

        if (ip_found) {
            // --- UDP Connection to NTP Server ---
            int sock_ntp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock_ntp < 0) {
                ESP_LOGE(TAG, "Failed to create UDP socket for NTP: errno %d", errno);
                // Will retry after delay at the end of the loop
            } else {
                // Set receive timeout
                struct timeval tv;
                tv.tv_sec = 2; // 2 second timeout
                tv.tv_usec = 0;
                setsockopt(sock_ntp, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

                // Setup server address
                struct sockaddr_in server_addr;
                memset(&server_addr, 0, sizeof(server_addr));
                server_addr.sin_family = AF_INET;
                server_addr.sin_addr.s_addr = inet_addr(ntp_server_address);
                server_addr.sin_port = htons(NTP_SERVER_PORT);
                
                // Prepare NTP request packet (48 bytes)
                uint8_t ntp_packet[48];
                memset(ntp_packet, 0, sizeof(ntp_packet));
                
                // Set the first byte: LI=0, Version=4, Mode=3 (client)
                ntp_packet[0] = 0x23; // 00100011 in binary
                
                // Record time before sending request
                struct timeval tv_before;
                gettimeofday(&tv_before, NULL);
                
                // Send NTP request
                if (sendto(sock_ntp, ntp_packet, sizeof(ntp_packet), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                    ESP_LOGE(TAG, "Failed to send to NTP server: errno %d", errno);
                    
                    // Increment failure count
                    dns_cache.failure_count++;
                    ESP_LOGD(TAG, "NTP send failure, failure count: %d/%d",
                             dns_cache.failure_count, MAX_FAILURE_COUNT);
                    
                    // Invalidate DNS cache after too many consecutive failures
                    if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                        dns_cache.valid = false;
                        dns_cache.failure_count = 0;
                        ESP_LOGW(TAG, "Invalidated DNS cache due to %d consecutive NTP send failures",
                                 MAX_FAILURE_COUNT);
                    }
                } else {
                    // Receive NTP response
                    uint8_t ntp_response[48];
                    socklen_t socklen = sizeof(server_addr);
                    int r = recvfrom(sock_ntp, ntp_response, sizeof(ntp_response), 0, (struct sockaddr*)&server_addr, &socklen);
                    
                    // Record time after receiving response
                    struct timeval tv_after;
                    gettimeofday(&tv_after, NULL);
                    
                    // Calculate round trip time with microsecond precision
                    int64_t round_trip_us = (tv_after.tv_sec - tv_before.tv_sec) * 1000000LL + 
                                           (tv_after.tv_usec - tv_before.tv_usec);
                    
                    // Convert to milliseconds for storage and logging
                    int32_t round_trip_ms = round_trip_us / 1000;
                    
                    //ESP_LOGD(TAG, "Round trip time: %lld us (%" PRId32 " ms) [before: %ld.%06ld, after: %ld.%06ld]",
                             //round_trip_us, round_trip_ms,
                             //(long)tv_before.tv_sec, (long)tv_before.tv_usec,
                             //(long)tv_after.tv_sec, (long)tv_after.tv_usec);
                    
                    if (r == sizeof(ntp_response)) {
                        // Extract the transmit timestamp (seconds and fraction) from the response
                        // NTP timestamp starts at byte 40 and is 8 bytes (4 for seconds, 4 for fraction)
                        uint32_t seconds_since_1900 = 
                            ((uint32_t)ntp_response[40] << 24) | 
                            ((uint32_t)ntp_response[41] << 16) | 
                            ((uint32_t)ntp_response[42] << 8) | 
                            (uint32_t)ntp_response[43];
                        
                        uint32_t fraction = 
                            ((uint32_t)ntp_response[44] << 24) | 
                            ((uint32_t)ntp_response[45] << 16) | 
                            ((uint32_t)ntp_response[46] << 8) | 
                            (uint32_t)ntp_response[47];
                        
                        // Convert fraction to microseconds (2^32 fraction = 1 second)
                        // microseconds = fraction * 1000000 / 2^32
                        int32_t microseconds = (int32_t)((double)fraction * 1000000.0 / 4294967296.0);
                        
                        // Convert NTP time (seconds since 1900) to Unix time (seconds since 1970)
                        // The difference is 70 years in seconds = 2208988800UL
                        time_t unix_time = seconds_since_1900 - 2208988800UL;
                        
                        ESP_LOGD(TAG, "Received NTP time: %lu.%06" PRId32 ", Unix time: %lu.%06" PRId32 "",
                                (unsigned long)seconds_since_1900, microseconds,
                                (unsigned long)unix_time, microseconds);
                        
                        // Reset failure count on successful NTP response
                        dns_cache.failure_count = 0;
                        
                        set_system_time(unix_time, microseconds, round_trip_ms);
                    } else if (r < 0) {
                        // Increment failure count
                        dns_cache.failure_count++;
                        
                        // Invalidate DNS cache after too many consecutive failures
                        if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                            dns_cache.valid = false;
                            dns_cache.failure_count = 0;
                        }
                    } else {
                        // Increment failure count
                        dns_cache.failure_count++;
                        
                        // Invalidate DNS cache after too many consecutive failures
                        if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                            dns_cache.valid = false;
                            dns_cache.failure_count = 0;
                        }
                    }
                }
                
                // Close UDP socket after attempt
                close(sock_ntp);
            }
        } else {
             // No IP found, will retry after delay
        }
        
        // Check if we have all 25 samples yet
        if (ntp_history.count < NTP_HISTORY_SIZE) {
            // Fast polling until we have all samples
            ESP_LOGD(TAG, "Fast polling mode: %d/%" PRId32 " samples collected",
                     ntp_history.count, (int32_t)NTP_HISTORY_SIZE);
            vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS); // Wait 0.05 seconds before next attempt
        } else if (!initial_sampling_complete) {
            // We just completed initial sampling
            ESP_LOGD(TAG, "Initial sampling complete with %" PRId32 " samples. Switching to normal polling rate.",
                     (int32_t)ntp_history.count);
            initial_sampling_complete = true;
            vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS); // Switch to normal polling interval
        } else {
            // Normal polling after we have all samples
            vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS); // Wait 0.5 seconds before next attempt
        }
    }

    // Cleanup (should not be reached in normal operation)
    vTaskDelete(NULL);
}

extern "C" void initialize_ntp_client() { // Ensure C linkage for app_main
    // Prevent multiple initializations
    if (ntp_client_initialized) {
        return;
    }
    
    // Create the NTP client task
    BaseType_t ret = xTaskCreatePinnedToCore(
        ntp_client_task,
        "ntp_client_task",
        4096,
        NULL,
        5,
        &ntp_client_task_handle,
        0
    );
    
    if (ret == pdPASS) {
        ntp_client_initialized = true;
    }
}

extern "C" void deinitialize_ntp_client() {
    if (!ntp_client_initialized || ntp_client_task_handle == NULL) {
        return;
    }
    
    // Delete the task
    vTaskDelete(ntp_client_task_handle);
    ntp_client_task_handle = NULL;
    ntp_client_initialized = false;
    
    // Clear DNS cache
    dns_cache.valid = false;
    dns_cache.failure_count = 0;
    memset(dns_cache.ip_address, 0, sizeof(dns_cache.ip_address));
}
