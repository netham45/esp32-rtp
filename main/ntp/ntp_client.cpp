#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h> // Required for errno
// #include <exception> // Not using C++ exceptions
// #include <stdexcept> // Not using C++ exceptions
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

#define DNS_MULTICAST_PORT 5353
#define DNS_MULTICAST_IPV4_ADDRESS "224.0.0.251"
#define NTP_SERVER_PORT 123
#define QUERY_TARGET "screamrouter.local"
#define NTP_HISTORY_SIZE 25 // Store last 25 results
#define NTP_POLL_INTERVAL_MS 500 // Poll every .5 seconds
#define NTP_FAST_POLL_INTERVAL_MS 50 // Poll every 0.05 seconds when building initial samples
#define MAX_FAILURE_COUNT 3 // Maximum number of consecutive failures before invalidating the cache

static const char *TAG = "ntp_client";

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

// DNS Header structure
typedef struct {
    uint16_t id;        // identification number
    uint16_t flags;     // DNS flags
    uint16_t qdcount;   // number of question entries
    uint16_t ancount;   // number of answer entries
    uint16_t nscount;   // number of authority entries
    uint16_t arcount;   // number of resource entries
} dns_header_t;

// DNS Question structure (simplified)
typedef struct {
    uint16_t qtype;
    uint16_t qclass;
} dns_question_tail_t;

// DNS Resource Record structure (simplified - focusing on A record)
typedef struct __attribute__((__packed__)) {
    uint16_t type;
    uint16_t rr_class; // Renamed from 'class'
    uint32_t ttl;
    uint16_t rdlength;
    // rdata follows
} dns_rr_fixed_tail_t;


// Function to format DNS name
static int format_dns_name(char *dest, const char *src) {
    int len = 0;
    char *label_start = (char *)src;
    while (1) {
        char *dot = strchr(label_start, '.');
        size_t label_len = dot ? (dot - label_start) : strlen(label_start);
        if (label_len > 63) return -1;
        *dest++ = (char)label_len;
        memcpy(dest, label_start, label_len);
        dest += label_len;
        len += (1 + label_len);
        if (!dot) break;
        label_start = dot + 1;
    }
    *dest++ = 0;
    len++;
    return len;
}

// Function to parse DNS name from response
static int parse_dns_name(const uint8_t *dns_packet, int offset, int max_len, char *dest_name, int dest_max_len) {
    int name_len = 0;
    int current_offset = offset;
    int original_offset = offset;
    int hops = 0;
    bool first_segment = true;
    bool jumped = false;

    while (current_offset < max_len && dns_packet[current_offset] != 0 && hops < 10) {
        uint8_t segment_len = dns_packet[current_offset];

        if ((segment_len & 0xC0) == 0xC0) { // Pointer
            if (current_offset + 1 >= max_len) return -1;
            uint16_t pointer_offset = ((segment_len & 0x3F) << 8) | dns_packet[current_offset + 1];
            if (pointer_offset >= max_len) return -1;

            int pointed_name_len = parse_dns_name(dns_packet, pointer_offset, max_len, dest_name, dest_max_len);
            if (pointed_name_len < 0) return -1;
            if (!jumped) {
                 original_offset = current_offset + 2;
                 jumped = true;
            }
             return 2;

        } else if ((segment_len & 0xC0) == 0x00) { // Normal label
            current_offset++;
            if (current_offset + segment_len > max_len) return -1;

            if (!first_segment) {
                if (name_len < dest_max_len - 1) dest_name[name_len++] = '.';
                else return -1;
            }
            if (name_len + segment_len >= dest_max_len) return -1;

            memcpy(dest_name + name_len, dns_packet + current_offset, segment_len);
            name_len += segment_len;
            current_offset += segment_len;
            first_segment = false;
        } else {
            return -1;
        }
         if (jumped) break;
    }

    if (!jumped) {
        if (current_offset >= max_len || dns_packet[current_offset] != 0) return -1;
        current_offset++;
    }

    dest_name[name_len] = '\0';
    return current_offset - original_offset;
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
        //ESP_LOGI(TAG, "Filtered %d/%d RTT outliers (bounds: [%" PRId32 ", %" PRId32 "] ms)", 
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
        settimeofday(&now, NULL);
        //ESP_LOGI(TAG, "System time set: %lld.%06" PRId32 " (using latest sample, adjusted for network delay)", 
                 //(long long)adjusted_time, adjusted_microseconds);
        //ESP_LOGI(TAG, "Time jitter: %.6f seconds, range: %lld seconds (min: %lld, max: %lld)", 
                 //time_jitter, (long long)range, (long long)min_time, (long long)max_time);
        //ESP_LOGI(TAG, "Network stats: median RTT: %" PRId32 " ms, one-way delay: %" PRId32 " us, network jitter: %.3f ms", 
                 //median_round_trip, one_way_delay_us, network_jitter);
    } else {
                //ESP_LOGI(TAG, "Added timestamp to history (%d/%" PRId32 " samples needed for jitter calculation)", 
                   //      ntp_history.count, (int32_t)NTP_HISTORY_SIZE);
    }
}

static void ntp_client_task(void *pvParameters) {
    char ntp_server_address[46] = {0};
    int sock_tcp = -1;
    int sock_udp = -1;
    struct sockaddr_in dest_addr_dns;
    struct sockaddr_in bind_addr;
    struct timeval tv_udp;
    struct ip_mreq imreq; // For joining multicast group
    
    // Flag to track if we've completed initial sampling
    bool initial_sampling_complete = false;

    // --- Main Loop ---
    while (1) {
        bool ip_found = false;
        sock_udp = -1; // Reset socket descriptor each loop
        sock_tcp = -1; // Reset socket descriptor each loop

        // Check if we have a valid DNS cache
        if (dns_cache.valid) {
            // Use cached IP address
            strncpy(ntp_server_address, dns_cache.ip_address, sizeof(ntp_server_address));
            //ESP_LOGI(TAG, "Using cached DNS result: %s", ntp_server_address);
            ip_found = true;
        } else {
            // --- DNS Query ---
            //ESP_LOGI(TAG, "Attempting DNS query for %s to %s:%" PRId32, QUERY_TARGET, DNS_MULTICAST_IPV4_ADDRESS, (int32_t)DNS_MULTICAST_PORT);

            // Create UDP socket
            sock_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock_udp < 0) {
                ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
                
                // Use appropriate delay based on sampling status
                if (ntp_history.count < NTP_HISTORY_SIZE) {
                    vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                } else {
                    vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                }
                continue; // Skip to next iteration
            }

            // Configure destination address
            memset(&dest_addr_dns, 0, sizeof(dest_addr_dns));
            dest_addr_dns.sin_family = AF_INET;
            dest_addr_dns.sin_port = htons(DNS_MULTICAST_PORT);
            if (inet_pton(AF_INET, DNS_MULTICAST_IPV4_ADDRESS, &dest_addr_dns.sin_addr) <= 0) {
                ESP_LOGE(TAG, "inet_pton failed for multicast address");
                close(sock_udp);
                
                // Use appropriate delay based on sampling status
                if (ntp_history.count < NTP_HISTORY_SIZE) {
                    vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                } else {
                    vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                }
                continue; // Skip to next iteration
            }

            // Allow socket reuse
            int enable = 1;
            if (setsockopt(sock_udp, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
                //ESP_LOGW(TAG, "setsockopt(SO_REUSEADDR) failed: errno %d", errno);
            }

            // Bind to allow receiving responses
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            bind_addr.sin_port = htons(DNS_MULTICAST_PORT);
            if (bind(sock_udp, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGE(TAG, "Failed to bind UDP socket: errno %d", errno);
            close(sock_udp);
            
            // Use appropriate delay based on sampling status
            if (ntp_history.count < NTP_HISTORY_SIZE) {
                vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
            } else {
                vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
            }
            continue; // Skip to next iteration
            }

            // Join multicast group
            imreq.imr_multiaddr.s_addr = inet_addr(DNS_MULTICAST_IPV4_ADDRESS);
            imreq.imr_interface.s_addr = htonl(INADDR_ANY); // Use default interface
            if (setsockopt(sock_udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(struct ip_mreq)) < 0) {
                ESP_LOGE(TAG, "setsockopt(IP_ADD_MEMBERSHIP) failed: errno %d", errno);
                // Continue anyway
            }

            // Set receive timeout
            tv_udp.tv_sec = 2; // 2 second timeout
            tv_udp.tv_usec = 0;
            if (setsockopt(sock_udp, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_udp, sizeof tv_udp) < 0) {
                ESP_LOGE(TAG, "setsockopt(SO_RCVTIMEO) failed: errno %d", errno);
            }

            // Construct DNS Query Packet
            uint8_t query_packet[512];
            memset(query_packet, 0, sizeof(query_packet));
            dns_header_t *dns_hdr = (dns_header_t *)query_packet;
            dns_hdr->id = htons(0x55AA); // Transaction ID
            dns_hdr->flags = htons(0x0100); // Standard query flags
            dns_hdr->qdcount = htons(1);    // One question

            char *qname_ptr = (char *)(query_packet + sizeof(dns_header_t));
            int name_len = format_dns_name(qname_ptr, QUERY_TARGET);
            if (name_len < 0) {
                ESP_LOGE(TAG, "Failed to format DNS name");
                close(sock_udp);
                
                // Use appropriate delay based on sampling status
                if (ntp_history.count < NTP_HISTORY_SIZE) {
                    vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                } else {
                    vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                }
                continue; // Skip to next iteration
            }

            dns_question_tail_t *qtail = (dns_question_tail_t *)(qname_ptr + name_len);
            qtail->qtype = htons(1);  // Type A
            qtail->qclass = htons(1); // Class IN

            int query_len = sizeof(dns_header_t) + name_len + sizeof(dns_question_tail_t);

            // Send DNS query via UDP multicast
            int sent_len = sendto(sock_udp, query_packet, query_len, 0, (struct sockaddr *)&dest_addr_dns, sizeof(dest_addr_dns));
            if (sent_len < 0) {
                ESP_LOGE(TAG, "sendto failed: errno %d", errno);
                close(sock_udp);
                
                // Use appropriate delay based on sampling status
                if (ntp_history.count < NTP_HISTORY_SIZE) {
                    vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                } else {
                    vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS);
                }
                continue; // Skip to next iteration
            } else {
                //ESP_LOGI(TAG, "Sent %" PRId32 " bytes of DNS query", (int32_t)sent_len);
            }

            // Receive and parse response
            uint8_t recv_buf[512];
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);

            for (int i = 0; i < 5; i++) {
                int len = recvfrom(sock_udp, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&source_addr, &socklen);
                if (len < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (i == 0) ESP_LOGD(TAG, "recvfrom timeout waiting for first packet");
                        break;
                    }
                    ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                    break;
                }

                if (len < sizeof(dns_header_t)) continue;
                dns_header_t *resp_hdr = (dns_header_t *)recv_buf;
                if (!(ntohs(resp_hdr->flags) & 0x8000)) continue;
                if (ntohs(resp_hdr->ancount) == 0) continue;

                int offset = sizeof(dns_header_t);
                for (int q = 0; q < ntohs(resp_hdr->qdcount); ++q) {
                    char temp_name[256];
                    int parsed_len = parse_dns_name(recv_buf, offset, len, temp_name, sizeof(temp_name));
                    if (parsed_len < 0) { offset = len; break; }
                    offset += parsed_len;
                    if (offset + sizeof(dns_question_tail_t) > len) { offset = len; break; }
                    offset += sizeof(dns_question_tail_t);
                }
                if (offset >= len) continue;

                for (int a = 0; a < ntohs(resp_hdr->ancount); ++a) {
                    char answer_name[256];
                    int parsed_name_len = parse_dns_name(recv_buf, offset, len, answer_name, sizeof(answer_name));
                    if (parsed_name_len < 0) { offset = len; break; }
                    offset += parsed_name_len;

                    if (offset + sizeof(dns_rr_fixed_tail_t) > len) { offset = len; break; }

                    dns_rr_fixed_tail_t *rr_tail_base = (dns_rr_fixed_tail_t *)(recv_buf + offset);
                    uint16_t rr_type = ntohs(rr_tail_base->type);
                    uint16_t rr_class_val = ntohs(rr_tail_base->rr_class); // Use renamed member
                    uint16_t rr_rdlength = ntohs(rr_tail_base->rdlength);

                    if (rr_type == 1 && (rr_class_val & 0x7FFF) == 1 && strcmp(answer_name, QUERY_TARGET) == 0) {
                        if (rr_rdlength == 4) {
                            if (offset + sizeof(dns_rr_fixed_tail_t) + rr_rdlength > len) { offset = len; break; }
                            struct in_addr ip_addr;
                            memcpy(&ip_addr.s_addr, recv_buf + offset + sizeof(dns_rr_fixed_tail_t), sizeof(uint32_t));
                            snprintf(ntp_server_address, sizeof(ntp_server_address), "%s", inet_ntoa(ip_addr));
                            //ESP_LOGI(TAG, "Resolved DNS query to IP address: %s", ntp_server_address);
                            
                            // Update DNS cache
                            strncpy(dns_cache.ip_address, ntp_server_address, sizeof(dns_cache.ip_address));
                            dns_cache.valid = true;
                            dns_cache.failure_count = 0; // Reset failure count on successful DNS resolution
                            //ESP_LOGI(TAG, "Updated DNS cache with IP: %s", dns_cache.ip_address);
                            
                            ip_found = true;
                            offset = len;
                            break;
                        }
                    }
                    offset += sizeof(dns_rr_fixed_tail_t) + rr_rdlength;
                    if (offset >= len) break;
                }
                if (ip_found) break;
            } // End receive loop

            // Close UDP socket after query attempt
            if (sock_udp >= 0) {
                // Leave multicast group (optional)
                // setsockopt(sock_udp, IPPROTO_IP, IP_DROP_MEMBERSHIP, &imreq, sizeof(struct ip_mreq));
                close(sock_udp);
                sock_udp = -1;
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
                if (setsockopt(sock_ntp, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
                    //ESP_LOGW(TAG, "setsockopt(SO_RCVTIMEO) failed for NTP socket: errno %d", errno);
                }

                // Setup server address
                struct sockaddr_in server_addr;
                memset(&server_addr, 0, sizeof(server_addr));
                server_addr.sin_family = AF_INET;
                server_addr.sin_addr.s_addr = inet_addr(ntp_server_address);
                server_addr.sin_port = htons(NTP_SERVER_PORT);

                //ESP_LOGI(TAG, "Sending request to NTP server %s:%d", ntp_server_address, NTP_SERVER_PORT);
                
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
                //ESP_LOGI(TAG, "NTP send failure, failure count: %d/%" PRId32, 
                          //   dns_cache.failure_count, (int32_t)MAX_FAILURE_COUNT);
                    
                    // Invalidate DNS cache after too many consecutive failures
                    if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                        dns_cache.valid = false;
                        dns_cache.failure_count = 0;
                        //ESP_LOGI(TAG, "Invalidated DNS cache due to too many NTP send failures");
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
                    
                    //ESP_LOGI(TAG, "Round trip time: %lld us (%" PRId32 " ms) [before: %ld.%06ld, after: %ld.%06ld]",
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
                        
                        //ESP_LOGI(TAG, "Received NTP time: %lu.%06" PRId32 ", Unix time: %lu.%06" PRId32 "", 
                                //(unsigned long)seconds_since_1900, microseconds, 
                                //(unsigned long)unix_time, microseconds);
                        
                        // Reset failure count on successful NTP response
                        dns_cache.failure_count = 0;
                        
                        set_system_time(unix_time, microseconds, round_trip_ms);
                    } else if (r < 0) {
                        ESP_LOGE(TAG, "Failed to receive time from NTP server: errno %d", errno);
                        
                        // Increment failure count
                        dns_cache.failure_count++;
                        //ESP_LOGI(TAG, "NTP receive failure, failure count: %d/%" PRId32, 
                                 //dns_cache.failure_count, (int32_t)MAX_FAILURE_COUNT);
                        
                        // Invalidate DNS cache after too many consecutive failures
                        if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                            dns_cache.valid = false;
                            dns_cache.failure_count = 0;
                            //ESP_LOGI(TAG, "Invalidated DNS cache due to too many NTP receive failures");
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to receive time: received %d bytes (expected %d)", r, sizeof(ntp_response));
                        
                        // Increment failure count
                        dns_cache.failure_count++;
                        //ESP_LOGI(TAG, "NTP receive failure (wrong size), failure count: %d/%" PRId32, 
                                 //dns_cache.failure_count, (int32_t)MAX_FAILURE_COUNT);
                        
                        // Invalidate DNS cache after too many consecutive failures
                        if (dns_cache.failure_count >= MAX_FAILURE_COUNT) {
                            dns_cache.valid = false;
                            dns_cache.failure_count = 0;
                            //ESP_LOGI(TAG, "Invalidated DNS cache due to too many NTP receive failures");
                        }
                    }
                }
                
                // Close UDP socket after attempt
                close(sock_ntp);
            }
        } else {
             //ESP_LOGW(TAG, "Could not resolve %s via DNS query to multicast", QUERY_TARGET);
             // No IP found, will retry after delay
        }

        // Ensure sockets are closed if an error occurred before this point in the loop
        if (sock_udp >= 0) {
            close(sock_udp);
            sock_udp = -1;
        }
        if (sock_tcp >= 0) {
            close(sock_tcp);
            sock_tcp = -1;
        }
        
        // Check if we have all 25 samples yet
        if (ntp_history.count < NTP_HISTORY_SIZE) {
            // Fast polling until we have all samples
            //ESP_LOGI(TAG, "Fast polling mode: %d/%" PRId32 " samples collected", 
                     //ntp_history.count, (int32_t)NTP_HISTORY_SIZE);
            vTaskDelay(NTP_FAST_POLL_INTERVAL_MS / portTICK_PERIOD_MS); // Wait 0.5 seconds before next attempt
        } else if (!initial_sampling_complete) {
            // We just completed initial sampling
            //ESP_LOGI(TAG, "Initial sampling complete with %" PRId32 " samples. Switching to normal polling rate.", 
                     //(int32_t)ntp_history.count);
            initial_sampling_complete = true;
            vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS); // Switch to normal polling interval
        } else {
            // Normal polling after we have all samples
            vTaskDelay(NTP_POLL_INTERVAL_MS / portTICK_PERIOD_MS); // Wait 5 seconds before next attempt
        }
    }

    // Cleanup (should not be reached in normal operation)
    if (sock_udp >= 0) close(sock_udp);
    if (sock_tcp >= 0) close(sock_tcp);
    vTaskDelete(NULL);
}

extern "C" void initialize_ntp_client() { // Ensure C linkage for app_main
    xTaskCreatePinnedToCore(ntp_client_task, "ntp_client_task", 4096, NULL, 2, NULL, 0); // Increased stack size
}
