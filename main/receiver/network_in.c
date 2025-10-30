// RTP packet structure: [12-byte RTP header] + [1152-byte PCM audio payload]
// Total packet size: 1164 bytes

#include "sdkconfig.h"
#include "build_config.h"
#include "network_in.h"
#include "global.h"

#include "audio_out.h"
#include "spdif_out.h"
#include "lifecycle_manager.h"
#ifdef CONFIG_RTCP_ENABLED
#include "rtcp_receiver.h"
#endif
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "buffer.h"
#include "esp_timer.h"
#include "config/config_manager.h"
#include "pcm_visualizer.h"  // For pcm_viz_write

// Low-rate summary interval default if not provided by Kconfig
#ifndef CONFIG_RTP_RX_LOG_SUMMARY_INTERVAL_MS
#define CONFIG_RTP_RX_LOG_SUMMARY_INTERVAL_MS 5000
#endif
/*
 * Runtime audio bytes-per-ms helper.
 * 192 bytes/ms is only true for 48kHz, 16-bit, 2-channel audio (48000 * 2 * 2 / 1000 = 192).
 * Using this helper avoids reintroducing that magic number and adapts to runtime configuration.
 */
static inline uint32_t rtp_bytes_per_ms(void) {
    uint32_t sample_rate = lifecycle_get_sample_rate();
    uint32_t bit_depth   = lifecycle_get_bit_depth();

    // TODO: If a runtime channels accessor becomes available, use it; default to stereo for now.
    uint32_t channels = 2;

    if (sample_rate == 0 || bit_depth == 0 || channels == 0) {
        return 0;
    }
    if ((bit_depth % 8u) != 0u) {
        // Non-byte-aligned bit depths not supported by this helper; avoid fractional bytes/sample.
        return 0;
    }

    uint32_t bytes_per_sample = bit_depth / 8u;
    uint64_t numerator = (uint64_t)sample_rate * (uint64_t)channels * (uint64_t)bytes_per_sample;
    return (uint32_t)(numerator / 1000u);
}
// RTP header structure (12 bytes) - matches sender format
typedef struct __attribute__((packed)) {
    uint8_t  vpxcc;      // Version(2), Padding(1), Extension(1), CSRC count(4)
    uint8_t  mpt;        // Marker(1), Payload Type(7)
    uint16_t seq_num;    // Sequence number
    uint32_t timestamp;  // RTP timestamp
    uint32_t ssrc;       // Synchronization source identifier
} rtp_header_t;

// Helper macros for parsing RTP header fields
#define RTP_VERSION(vpxcc)    ((vpxcc) >> 6)
#define RTP_PADDING(vpxcc)    (((vpxcc) >> 5) & 0x01)
#define RTP_EXTENSION(vpxcc)  (((vpxcc) >> 4) & 0x01)
#define RTP_CC(vpxcc)         ((vpxcc) & 0x0F)
#define RTP_MARKER(mpt)       ((mpt) >> 7)
#define RTP_PT(mpt)           ((mpt) & 0x7F)

// Network constants
#define UDP_PORT CONFIG_RTP_PORT
#define MAX_RTP_PACKET_SIZE (sizeof(rtp_header_t) + (15 * 4) + PCM_CHUNK_SIZE + 512)  // Max: 12 + 60 (CSRCs) + audio + 512 (padding/extensions)
 // PCM chunk size provided by global.h (PCM_CHUNK_SIZE)
// SAP functionality moved to sap_listener.c

// Socket and task handles
static int unicast_sock = -1;  // Socket for configured port (always active)
static int multicast_sock = -1;  // Socket for multicast groups (when in multicast mode)
#ifdef CONFIG_RTCP_ENABLED
static int rtcp_sock = -1;  // Socket for RTCP packets (port + 1)
#endif
static TaskHandle_t udp_handler_task = NULL;

// Accumulator to repackage arbitrary RTP payload sizes into PCM_CHUNK_SIZE chunks
// Avoids dropping "non-standard" payload sizes by buffering and emitting exact 1152-byte blocks.
static uint8_t  agg_buf[PCM_CHUNK_SIZE];
static uint16_t agg_len = 0;              // Current fill length in bytes
static uint32_t agg_rtp_start_ts = 0;     // RTP timestamp corresponding to agg_buf[0]
static uint32_t agg_last_ssrc = 0;        // Guard to avoid mixing sources across accumulator

// RTP statistics
static uint32_t packets_received = 0;
static uint32_t packets_lost = 0;
// Kept for legacy stats accessor; no longer updated in RTCP path
static uint32_t packets_dropped_late = 0;

// Track enqueue path usage (RTCP mapped vs legacy fallback)
static uint32_t mapped_enqueue_count = 0;
static uint32_t legacy_enqueue_count = 0;
// Multicast configuration
typedef struct {
    bool enabled;
    char multicast_ip[16];
    uint16_t port;
    uint32_t ssrc_filter;
    bool filter_by_ssrc;
    struct ip_mreq mreq;
} multicast_config_t;

static multicast_config_t multicast_config = {
    .enabled = false,
    .multicast_ip = "",
    .port = 0,
    .ssrc_filter = 0,
    .filter_by_ssrc = false
};

static void create_udp_server(void) {
    app_config_t* config = config_manager_get_config();
    uint16_t port = config ? config->port : UDP_PORT;

    struct sockaddr_in dest_addr;

    unicast_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (unicast_sock < 0) {
        ESP_LOGE(TAG, "Unable to create unicast socket: errno %d", errno);
        return;
    }

    // Set socket options
    int reuse = 1;
    setsockopt(unicast_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Set non-blocking
    int flags = fcntl(unicast_sock, F_GETFL, 0);
    fcntl(unicast_sock, F_SETFL, flags | O_NONBLOCK);

    // Bind to UDP port
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_port = htons(port);

    int err = bind(unicast_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Unicast socket unable to bind: errno %d", errno);
        close(unicast_sock);
        unicast_sock = -1;
        return;
    }

    ESP_LOGI(TAG, "UDP server listening on port %d (unicast socket)", port);
    
#ifdef CONFIG_RTCP_ENABLED
    // Create RTCP socket on port + 1
    rtcp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (rtcp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create RTCP socket: errno %d", errno);
        return;
    }
    
    // Set socket options
    setsockopt(rtcp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Set non-blocking
    flags = fcntl(rtcp_sock, F_GETFL, 0);
    fcntl(rtcp_sock, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to RTCP port (RTP port + 1)
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_port = htons(port + 1);
    
    err = bind(rtcp_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "RTCP socket unable to bind to port %d: errno %d", port + 1, errno);
        close(rtcp_sock);
        rtcp_sock = -1;
        return;
    }
    
    ESP_LOGI(TAG, "RTCP server listening on port %d", port + 1);
#endif
}

static void close_udp_server(void) {
    if (unicast_sock >= 0) {
        close(unicast_sock);
        unicast_sock = -1;
    }
#ifdef CONFIG_RTCP_ENABLED
    if (rtcp_sock >= 0) {
        close(rtcp_sock);
        rtcp_sock = -1;
    }
#endif
}

static void close_multicast_socket(void) {
    if (multicast_sock >= 0) {
        close(multicast_sock);
        multicast_sock = -1;
    }
}
// Low-rate RTP structured summary; prints once per CONFIG_RTP_RX_LOG_SUMMARY_INTERVAL_MS
static void rtp_log_summary_if_due(void) {
#ifndef CONFIG_RTCP_LOG_RX_STATS
    return;
#else
    static uint64_t last_sum_us = 0;
    const uint64_t interval_us = ((uint64_t)CONFIG_RTP_RX_LOG_SUMMARY_INTERVAL_MS) * 1000ULL;
    uint64_t now = esp_timer_get_time();
    if (last_sum_us != 0 && (now - last_sum_us) < interval_us) {
        return;
    }
    last_sum_us = now;

    const char *mode_str = multicast_config.enabled ? "Multicast" : "Unicast";
    uint32_t jitter_us = 0;
    int32_t cumlost = 0;
    uint32_t primary = 0;
    bool have_primary = false;

#ifdef CONFIG_RTCP_ENABLED
    double jitter_ts = 0.0;
    if (rtcp_get_primary_ssrc(&primary)) {
        have_primary = rtcp_get_rx_stats(primary, NULL, &cumlost, &jitter_ts);
        if (have_primary && jitter_ts > 0.0) {
            // Convert RTP tick jitter to microseconds using nominal a0
            double j_us = jitter_ts * (1000000.0 / (double)CONFIG_SAMPLE_RATE);
            if (j_us < 0.0) j_us = 0.0;
            if (j_us > (double)UINT32_MAX) j_us = (double)UINT32_MAX;
            jitter_us = (uint32_t)(j_us + 0.5);
        }
    }
#endif

    ESP_LOGI(TAG,
             "RTP sum: rx=%u lost=%u drop=%u mode=%s filter=%d mapped=%u legacy=%u jitter_us=%u cumlost=%d ssrc=0x%08X",
             packets_received,
             packets_lost,
             packets_dropped_late,
             mode_str,
             multicast_config.filter_by_ssrc ? 1 : 0,
             mapped_enqueue_count,
             legacy_enqueue_count,
             jitter_us,
             (int)cumlost,
             have_primary ? primary : 0u);
#endif
}

// SAP handler moved to sap_listener.c

static void udp_handler(void *pvParameters) {
    // RTP packet buffer - allocate enough for maximum possible RTP packet
    char rx_buffer[MAX_RTP_PACKET_SIZE];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    fd_set read_fds;
    struct timeval tv;

    while (1) {
        // Periodic RTP summary (low rate)
        rtp_log_summary_if_due();

        // Setup select with both sockets
        FD_ZERO(&read_fds);
        int max_fd = -1;
        
        if (unicast_sock >= 0) {
            FD_SET(unicast_sock, &read_fds);
            max_fd = unicast_sock;
        }
        
        if (multicast_sock >= 0) {
            FD_SET(multicast_sock, &read_fds);
            if (multicast_sock > max_fd) {
                max_fd = multicast_sock;
            }
        }
        
#ifdef CONFIG_RTCP_ENABLED
        if (rtcp_sock >= 0) {
            FD_SET(rtcp_sock, &read_fds);
            if (rtcp_sock > max_fd) {
                max_fd = rtcp_sock;
            }
        }
#endif
        
        // If no sockets are active, wait and retry
        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int select_result = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        
        if (select_result < 0) {
            ESP_LOGE(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else if (select_result == 0) {
            // Timeout, no data available - yield to other tasks
            vTaskDelay(0);
            continue;
        }

        // Check which socket has data and read from it
        int active_sock = -1;
        bool is_rtcp = false;
        
        if (unicast_sock >= 0 && FD_ISSET(unicast_sock, &read_fds)) {
            active_sock = unicast_sock;
        } else if (multicast_sock >= 0 && FD_ISSET(multicast_sock, &read_fds)) {
            active_sock = multicast_sock;
        }
#ifdef CONFIG_RTCP_ENABLED
        else if (rtcp_sock >= 0 && FD_ISSET(rtcp_sock, &read_fds)) {
            active_sock = rtcp_sock;
            is_rtcp = true;
        }
#endif
        
        if (active_sock < 0) {
            continue;  // No data available
        }

        // Data is available, read it
        int len = recvfrom(active_sock, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

#ifdef CONFIG_RTCP_ENABLED
        // Check if this is an RTCP packet
        if (is_rtcp) {
            ESP_LOGI(TAG, "Received RTCP packet: %d bytes from %s:%d",
                    len, inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port));
            
            // Parse RTCP packet
            if (rtcp_parse_packet((uint8_t *)rx_buffer, len) == ESP_OK) {
                ESP_LOGI(TAG, "RTCP packet parsed successfully");
            }
            continue;  // RTCP packets don't contain audio data
        }
#endif

        if (len < sizeof(rtp_header_t)) {
            ESP_LOGW(TAG, "Packet too small for RTP header: %d bytes", len);
            continue;
        }

        // Parse RTP header
        rtp_header_t *rtp = (rtp_header_t *)rx_buffer;

        // Validate RTP version (should be 2)
        uint8_t version = RTP_VERSION(rtp->vpxcc);
        if (version != 2) {
            ESP_LOGW(TAG, "Invalid RTP version: %d (vpxcc=0x%02X)", version, rtp->vpxcc);
            // Log first few bytes of packet for debugging
            ESP_LOGW(TAG, "First 16 bytes of packet:");
            for (int i = 0; i < 16 && i < len; i++) {
                ESP_LOGW(TAG, "  [%02d]: 0x%02X", i, (uint8_t)rx_buffer[i]);
            }
            continue;
        }

        // Filter by SSRC if in multicast mode
        if (multicast_config.enabled && multicast_config.filter_by_ssrc) {
            uint32_t packet_ssrc = ntohl(rtp->ssrc);
            if (packet_ssrc != multicast_config.ssrc_filter) {
                static uint32_t filtered_count = 0;
                static uint32_t last_logged_ssrc = 0;
                filtered_count++;

                // Log different SSRCs we're filtering out (but not too frequently)
                if (packet_ssrc != last_logged_ssrc || filtered_count % 1000 == 0) {
                    ESP_LOGD(TAG, "Filtering out packet with SSRC 0x%08X (expected 0x%08X), filtered=%u",
                            packet_ssrc, multicast_config.ssrc_filter, filtered_count);
                    last_logged_ssrc = packet_ssrc;
                }
                continue;
            }
        }

        // Calculate actual header size based on CSRC count
        uint8_t cc = RTP_CC(rtp->vpxcc);  // CSRC count (0-15)
        uint8_t has_extension = RTP_EXTENSION(rtp->vpxcc);
        uint8_t has_padding = RTP_PADDING(rtp->vpxcc);
        
        int header_size = sizeof(rtp_header_t) + (cc * 4);  // 12 bytes + 4 bytes per CSRC
        
        if (len < header_size) {
            ESP_LOGW(TAG, "Packet too small for RTP header with %d CSRCs: %d bytes", cc, len);
            continue;
        }
        
        // Handle RTP header extension if present
        if (has_extension) {
            if (len < header_size + 4) {  // Need at least 4 bytes for extension header
                ESP_LOGW(TAG, "Packet too small for RTP extension header");
                continue;
            }
            // Extension header: 16-bit profile + 16-bit length (in 32-bit words)
            uint8_t *ext_ptr = (uint8_t *)&rx_buffer[header_size];
            uint16_t ext_length = ntohs(*(uint16_t *)(ext_ptr + 2));
            header_size += 4 + (ext_length * 4);  // Add extension header + extension data
            
            if (len < header_size) {
                ESP_LOGW(TAG, "Packet too small for RTP extension data");
                continue;
            }
        }

        // Track sequence numbers and update RTCP RX stats
#ifdef CONFIG_RTCP_ENABLED
        // Capture arrival time as soon as possible and convert to RTP tick units
        uint64_t arrival_mono_us = esp_timer_get_time();
        uint32_t arrival_rtp_ticks = (uint32_t)((arrival_mono_us * (uint64_t)CONFIG_SAMPLE_RATE) / 1000000ULL);
#endif
        static uint16_t last_seq = 0;
        static bool first_packet = true;
        uint16_t seq = ntohs(rtp->seq_num);
#ifdef CONFIG_RTCP_ENABLED
        // Update RTCP-backed receiver stats (extended seq, loss, jitter)
        uint32_t ssrc = ntohl(rtp->ssrc);
        uint32_t rtp_ts = ntohl(rtp->timestamp);
        rtcp_update_rx_stats(ssrc, seq, rtp_ts, arrival_rtp_ticks);

        // Primary SSRC hygiene (decimated): consider switching when not filtering by SSRC
        static uint32_t primary_decimator = 0;
        if (!multicast_config.filter_by_ssrc) {
            if ((++primary_decimator & 0xFFu) == 0) {
                uint32_t new_primary = 0;
                if (rtcp_consider_primary_switch(ssrc, &new_primary)) {
#ifdef CONFIG_RTCP_LOG_SSRC
                    ESP_LOGI(TAG, "Primary SSRC switched to 0x%08X by RX activity", new_primary);
#endif
                }
            }
        }
#else
        // Legacy local loss tracking (disabled when RTCP is enabled to avoid double counting)
        if (!first_packet) {
            uint16_t expected_seq = (last_seq + 1) & 0xFFFF;
            if (seq != expected_seq) {
                int lost = (seq - expected_seq) & 0xFFFF;
                if (lost < 1000) {  // Reasonable threshold for loss vs reordering
                    packets_lost += lost;
                    ESP_LOGW(TAG, "Packet loss detected: expected seq %u, got %u (lost %d)",
                            expected_seq, seq, lost);
                }
            }
        } else {
            first_packet = false;
        }
        last_seq = seq;
#endif
        packets_received++;
        
#ifdef CONFIG_RTCP_ENABLED
        // RTCP RX stats were updated earlier with accurate arrival time
#endif

        // Calculate payload length
        int payload_len = len - header_size;
        
        // Handle padding if present
        if (has_padding && payload_len > 0) {
            // Last byte of payload contains padding length
            uint8_t padding_len = rx_buffer[len - 1];
            if (padding_len > payload_len) {
                ESP_LOGW(TAG, "Invalid padding length: %d (payload_len=%d)", padding_len, payload_len);
                continue;
            }
            payload_len -= padding_len;
        }
        
        // Validate payload size (allow some flexibility but warn if unusual)
        if (payload_len <= 0) {
            ESP_LOGW(TAG, "No audio payload in RTP packet");
            continue;
        }
        
        if (payload_len != PCM_CHUNK_SIZE) {
            // Log as info instead of warning if it's a reasonable audio size
            if (payload_len % 4 == 0 && payload_len > 100 && payload_len < 8192) {
                ESP_LOGD(TAG, "Non-standard payload size: %d bytes (expected %d), header_size=%d, CSRCs=%d",
                        payload_len, PCM_CHUNK_SIZE, header_size, cc);
            } else {
                ESP_LOGW(TAG, "Unexpected payload size: %d bytes (expected %d), header_size=%d, CSRCs=%d",
                        payload_len, PCM_CHUNK_SIZE, header_size, cc);
                continue;
            }
        }
        
        // Extract audio data pointer
        uint8_t *audio_data = (uint8_t *)&rx_buffer[header_size];
        
        // Convert from network byte order (big-endian) to host byte order (little-endian)
        // RTP payload is s16be (signed 16-bit big-endian), ESP32 expects s16le
        uint16_t *samples = (uint16_t *)audio_data;
        int num_samples = payload_len / 2;  // 2 bytes per sample
        
        // Convert all samples from network byte order to host byte order
        for (int i = 0; i < num_samples; i++) {
            samples[i] = ntohs(samples[i]);
        }
        
        // Unified accumulator-based enqueue to handle arbitrary payload splits and emit 1152-byte chunks
        {
            // Compute alignment by audio frame (channels=2, bytes_per_sample from runtime bit depth)
            uint32_t bit_depth = lifecycle_get_bit_depth();
            uint32_t channels = 2; // default stereo unless accessor exists
            if (bit_depth == 0) {
                ESP_LOGW(TAG, "Invalid bit depth 0, dropping payload");
                continue;
            }
            uint32_t bytes_per_sample = (bit_depth / 8u);
            if (bytes_per_sample == 0) {
                ESP_LOGW(TAG, "Unsupported bit depth %u", bit_depth);
                continue;
            }
            uint32_t bpf = bytes_per_sample * channels; // bytes per interleaved frame

            // Require frame alignment; drop malformed payloads
            if (((uint32_t)payload_len % bpf) != 0u) {
                ESP_LOGW(TAG, "Payload not aligned to frame size: payload=%d, bpf=%u (dropping)", payload_len, bpf);
                continue;
            }

            // Track SSRC changes to avoid mixing different sources in the accumulator
            uint32_t ssrc2 = ntohl(rtp->ssrc);
            if (agg_len > 0 && agg_last_ssrc != 0 && ssrc2 != agg_last_ssrc) {
                ESP_LOGW(TAG, "SSRC changed 0x%08X -> 0x%08X; resetting accumulator (%u bytes discarded)",
                         agg_last_ssrc, ssrc2, (unsigned)agg_len);
                agg_len = 0;
            }
            agg_last_ssrc = ssrc2;

            uint32_t rtp_ts2 = ntohl(rtp->timestamp);

            int bytes_remaining = payload_len;
            int packet_offset = 0;

            while (bytes_remaining > 0) {
                // When starting a new 1152-byte chunk, compute the RTP timestamp for its first frame
                if (agg_len == 0) {
                    if (bpf > 0) {
                        uint32_t frames_offset = (uint32_t)(packet_offset / (int)bpf);
                        agg_rtp_start_ts = rtp_ts2 + frames_offset;
                    } else {
                        agg_rtp_start_ts = rtp_ts2;
                    }
                }

                int to_copy = (int)PCM_CHUNK_SIZE - (int)agg_len;
                if (to_copy > bytes_remaining) {
                    to_copy = bytes_remaining;
                }

                memcpy(&agg_buf[agg_len], &audio_data[packet_offset], (size_t)to_copy);
                agg_len += (uint16_t)to_copy;
                packet_offset += to_copy;
                bytes_remaining -= to_copy;

                if (agg_len >= PCM_CHUNK_SIZE) {
                    // We have one full 1152-byte chunk ready
                    pcm_viz_write(agg_buf, PCM_CHUNK_SIZE);

#ifdef CONFIG_RTCP_ENABLED
                    uint64_t playout_time = 0;
                    if (rtcp_calculate_playout_time(ssrc2, agg_rtp_start_ts, &playout_time) == ESP_OK) {
                        // PLL observation identical to the exact-size path
                        uint32_t sample_rate = lifecycle_get_sample_rate();
                        uint32_t bytes_per_sec = sample_rate * channels * bytes_per_sample;
                        if (bytes_per_sec > 0u) {
                            uint64_t packet_dur_us = ((uint64_t)PCM_CHUNK_SIZE * 1000000ULL) / (uint64_t)bytes_per_sec;
                            int64_t error_us = ((int64_t)playout_time - (int64_t)esp_timer_get_time()) - (int64_t)packet_dur_us;
                            rtcp_pll_observe(ssrc2, error_us, (uint32_t)packet_dur_us);
                        }
                        push_chunk_with_timestamp(agg_buf, playout_time);
                        mapped_enqueue_count++;
                        static uint32_t rtcp_sync_counter = 0;
                        if ((++rtcp_sync_counter % 1000u) == 0u) {
                            int64_t delta_us = (int64_t)playout_time - (int64_t)esp_timer_get_time();
                            ESP_LOGI(TAG, "RTCP sync: enqueued packet with playout in %lld ms",
                                     (long long)(delta_us / 1000));
                        }
                    } else {
                        push_chunk(agg_buf);
                        legacy_enqueue_count++;
                    }
#else
                    push_chunk(agg_buf);
                    legacy_enqueue_count++;
#endif
                    agg_len = 0; // Reset for next chunk (may be completed by current packet remainder)
                }
            }
        }
        
        // Log statistics periodically
        static uint32_t stats_counter = 0;
        if (++stats_counter >= 1000) {
            float loss_rate = 0.0f;
            uint32_t total = packets_received + packets_lost;
            if (total > 0) {
                loss_rate = (float)packets_lost / (float)total * 100.0f;
            }
            ESP_LOGI(TAG, "RTP RX Stats: Received=%u, Lost=%u (%.2f%%), Mode=%s",
                    packets_received, packets_lost, loss_rate,
                    multicast_config.enabled ? "Multicast" : "Unicast");
            stats_counter = 0;
        }
    }
    
    vTaskDelete(NULL);
}

esp_err_t network_init(void) {
    ESP_LOGI(TAG, "Starting network receiver (RTP mode)");
    
#ifdef CONFIG_RTCP_ENABLED
    // Initialize RTCP receiver
    if (rtcp_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize RTCP receiver");
    }
#endif
    
    create_udp_server();

    xTaskCreatePinnedToCore(udp_handler, "udp_handler", 8192, NULL,
                           5, &udp_handler_task, 1);

    // SAP listener functionality moved to sap_listener module
    // It will be started by the lifecycle manager

    ESP_LOGI(TAG, "Network receiver started");
    return ESP_OK;
}

esp_err_t restart_network(void) {
    ESP_LOGI(TAG, "Restarting network (RTP/UDP)...");
    close_udp_server();
    create_udp_server();
    ESP_LOGI(TAG, "Network restarted");
    return ESP_OK;
}

esp_err_t network_update_port(void) {
    app_config_t* config = config_manager_get_config();
    if (!config) {
        ESP_LOGE(TAG, "Failed to get configuration");
        return ESP_FAIL;
    }
    
    uint16_t new_port = config->port;
    
    ESP_LOGI(TAG, "Updating network port to %d", new_port);
    
    // Close existing socket
    close_udp_server();
    
    // Brief delay to ensure socket is fully closed
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Create new socket with updated port
    create_udp_server();

    ESP_LOGI(TAG, "Network port successfully updated to %d", new_port);
    return ESP_OK;
}

esp_err_t network_join_multicast(const char* multicast_ip, uint16_t port, uint32_t ssrc) {
    if (!multicast_ip) {
        ESP_LOGE(TAG, "Invalid multicast IP");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if we're already connected to this exact multicast group with same parameters
    if (multicast_config.enabled &&
        strcmp(multicast_config.multicast_ip, multicast_ip) == 0 &&
        multicast_config.port == port &&
        multicast_config.ssrc_filter == ssrc) {
        ESP_LOGI(TAG, "Already connected to multicast group %s:%d with SSRC 0x%08X - skipping rejoin",
                multicast_ip, port, ssrc);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Joining multicast group %s:%d with SSRC filter 0x%08X", multicast_ip, port, ssrc);

    // Leave current multicast group if already joined
    if (multicast_config.enabled) {
        network_leave_multicast();
    }

    // Store new configuration
    strncpy(multicast_config.multicast_ip, multicast_ip, sizeof(multicast_config.multicast_ip) - 1);
    multicast_config.multicast_ip[sizeof(multicast_config.multicast_ip) - 1] = '\0';
    multicast_config.port = port;
    multicast_config.ssrc_filter = ssrc;
    multicast_config.filter_by_ssrc = true;

    // Close existing multicast socket if any
    close_multicast_socket();

    // Create new multicast socket
    multicast_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (multicast_sock < 0) {
        ESP_LOGE(TAG, "Unable to create multicast socket: errno %d", errno);
        return ESP_FAIL;
    }

    // Set socket options for multicast
    int reuse = 1;
    setsockopt(multicast_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Set non-blocking
    int flags = fcntl(multicast_sock, F_GETFL, 0);
    fcntl(multicast_sock, F_SETFL, flags | O_NONBLOCK);

    // Bind to multicast port (use INADDR_ANY to receive multicast)
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_port = htons(port);

    int err = bind(multicast_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Multicast socket unable to bind to port %d: errno %d", port, errno);
        close(multicast_sock);
        multicast_sock = -1;
        return ESP_FAIL;
    }

    // Join multicast group
    multicast_config.mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
    multicast_config.mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    err = setsockopt(multicast_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &multicast_config.mreq, sizeof(multicast_config.mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to join multicast group %s: errno %d", multicast_ip, errno);
        close(multicast_sock);
        multicast_sock = -1;
        return ESP_FAIL;
    }

    multicast_config.enabled = true;
    ESP_LOGI(TAG, "Successfully joined multicast group %s:%d (unicast socket remains active)", multicast_ip, port);

    return ESP_OK;
}

esp_err_t network_leave_multicast(void) {
    if (!multicast_config.enabled) {
        ESP_LOGW(TAG, "Not currently in multicast mode");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Leaving multicast group %s", multicast_config.multicast_ip);

    // Leave multicast group
    if (multicast_sock >= 0) {
        int err = setsockopt(multicast_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                            &multicast_config.mreq, sizeof(multicast_config.mreq));
        if (err < 0) {
            ESP_LOGW(TAG, "Failed to leave multicast group: errno %d", errno);
        }
    }

    // Close multicast socket
    close_multicast_socket();

    // Reset multicast configuration
    multicast_config.enabled = false;
    multicast_config.filter_by_ssrc = false;
    multicast_config.ssrc_filter = 0;
    memset(multicast_config.multicast_ip, 0, sizeof(multicast_config.multicast_ip));

    ESP_LOGI(TAG, "Left multicast group (unicast socket remains active)");
    return ESP_OK;
}

bool network_is_multicast_enabled(void) {
    return multicast_config.enabled;
}

void network_get_multicast_info(char* ip, uint16_t* port, uint32_t* ssrc) {
    if (ip) {
        strcpy(ip, multicast_config.multicast_ip);
    }
    if (port) {
        *port = multicast_config.port;
    }
    if (ssrc) {
        *ssrc = multicast_config.ssrc_filter;
    }
}

// Check if an IP address is in the multicast range (224.0.0.0 - 239.255.255.255)
static bool is_multicast_ip(const char* ip) {
    if (!ip) return false;

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return false;
    }

    uint32_t ip_num = ntohl(addr.s_addr);
    uint8_t first_octet = (ip_num >> 24) & 0xFF;

    // Multicast addresses are in the range 224.0.0.0 to 239.255.255.255
    return (first_octet >= 224 && first_octet <= 239);
}

// Check if an IP address matches one of our local interface IPs
static bool is_local_interface_ip(const char* ip) {
    if (!ip) return false;

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = NULL;

    // Check WiFi STA interface
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char local_ip[16];
            esp_ip4addr_ntoa(&ip_info.ip, local_ip, sizeof(local_ip));
            if (strcmp(local_ip, ip) == 0) {
                return true;
            }
        }
    }

    // Check WiFi AP interface (if active)
    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char local_ip[16];
            esp_ip4addr_ntoa(&ip_info.ip, local_ip, sizeof(local_ip));
            if (strcmp(local_ip, ip) == 0) {
                return true;
            }
        }
    }

    // Check Ethernet interface (if available)
    netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif) {
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char local_ip[16];
            esp_ip4addr_ntoa(&ip_info.ip, local_ip, sizeof(local_ip));
            if (strcmp(local_ip, ip) == 0) {
                return true;
            }
        }
    }

    return false;
}

esp_err_t network_configure_stream(const char* dest_ip, const char* source_ip, uint16_t port) {
    if (!dest_ip || !source_ip) {
        ESP_LOGE(TAG, "Invalid parameters for stream configuration");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Configuring stream: dest=%s, source=%s, port=%d", dest_ip, source_ip, port);

    // Check if destination IP is multicast
    if (is_multicast_ip(dest_ip)) {
        ESP_LOGI(TAG, "Destination is multicast address, joining multicast group");

        // Generate SSRC from source IP and port for filtering
        // This helps filter packets when multiple sources send to the same multicast group
        uint32_t ssrc = 0;
        int last_octet = 0;
        if (sscanf(source_ip, "%*d.%*d.%*d.%d", &last_octet) == 1) {
            ssrc = (last_octet << 16) | port;
        }

        return network_join_multicast(dest_ip, port, ssrc);
    } else if (is_local_interface_ip(dest_ip)) {
        ESP_LOGI(TAG, "Destination is local interface address, unicast reception already configured");

        // For unicast to our local IP, the configured port socket is already listening
        // Just leave any existing multicast group if active
        if (multicast_config.enabled) {
            network_leave_multicast();
        }

        // Note: We do NOT update the port from SAP announcements
        // The configured port socket remains on its configured port
        ESP_LOGI(TAG, "Unicast socket listening on configured port, SAP announcement port (%d) ignored", port);

        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Destination IP %s is neither a local interface IP nor a multicast address - ignoring configuration", dest_ip);
        return ESP_ERR_INVALID_ARG;
    }
}

void get_rtp_statistics(uint32_t *received, uint32_t *lost, float *loss_rate) {
    if (received) {
        *received = packets_received;
    }
    if (lost) {
        *lost = packets_lost;
    }
    if (loss_rate) {
        uint32_t total = packets_received + packets_lost;
        if (total > 0) {
            *loss_rate = (float)packets_lost / (float)total * 100.0f;
        } else {
            *loss_rate = 0.0f;
        }
    }
}

void get_rtp_drop_statistics(uint32_t *dropped, float *drop_rate) {
    if (dropped) {
        *dropped = packets_dropped_late;
    }
    if (drop_rate) {
        if (packets_received > 0) {
            *drop_rate = (float)packets_dropped_late / (float)packets_received * 100.0f;
        } else {
            *drop_rate = 0.0f;
        }
    }
}

esp_err_t network_deinit(void) {
    ESP_LOGI(TAG, "Stopping network receiver");
    
    if (udp_handler_task) {
        vTaskDelete(udp_handler_task);
        udp_handler_task = NULL;
    }
    
    close_udp_server();
    close_multicast_socket();
    
#ifdef CONFIG_RTCP_ENABLED
    rtcp_deinit();
#endif
    
    ESP_LOGI(TAG, "Network receiver stopped");
    return ESP_OK;
}
