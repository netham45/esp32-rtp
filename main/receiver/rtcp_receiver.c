#include "rtcp_receiver.h"
#include "rtcp_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include "sdkconfig.h"

#define TAG "RTCP_RX"

#ifndef CONFIG_RTCP_LOG_SYNC_INFO
#define CONFIG_RTCP_LOG_SYNC_INFO true
#endif


// Use Kconfig values or defaults
#ifdef CONFIG_RTCP_RR_INTERVAL_MS
#define RTCP_RR_INTERVAL_MS CONFIG_RTCP_RR_INTERVAL_MS
#else
#define RTCP_RR_INTERVAL_MS 5000
#endif

#ifdef CONFIG_RTCP_TARGET_LATENCY_MS
#define RTCP_TARGET_LATENCY_MS CONFIG_RTCP_TARGET_LATENCY_MS
#else
#define RTCP_TARGET_LATENCY_MS 50
#endif

#define RTCP_BUFFER_SIZE 512  // RTCP packets are typically small

// Module state
typedef struct {
    // Sockets
    int unicast_sock;
    int multicast_sock;
    
    // Multicast config
    bool multicast_enabled;
    struct ip_mreq mreq;
    char multicast_ip[16];
    
    // Task handle
    TaskHandle_t task_handle;
    bool running;
    
    // Synchronization info
    rtcp_sync_info_t sync_info;
    
    // Our receiver SSRC
    uint32_t receiver_ssrc;
    
    // RTP statistics (updated from network_in.c)
    uint32_t rtp_packets_received;
    uint32_t rtp_packets_lost;
    uint16_t rtp_last_seq;
    uint32_t rtp_last_timestamp;
    
    // Jitter calculation
    rtcp_jitter_calc_t jitter_calc;
    
    // RTCP statistics
    rtcp_stats_t stats;
    
    // Source address for RR
    struct sockaddr_in source_addr;
    bool source_addr_valid;
    
    // Last RR sent time
    uint64_t last_rr_time;
    
    // Configuration
    uint16_t base_rtp_port;
} rtcp_receiver_state_t;

static rtcp_receiver_state_t s_state = {
    .unicast_sock = -1,
    .multicast_sock = -1,
    .multicast_enabled = false,
    .running = false,
    .source_addr_valid = false,
};

// Forward declarations
static void rtcp_receiver_task(void *arg);
static esp_err_t create_rtcp_socket(uint16_t port);
static void process_rtcp_packet(const uint8_t *data, size_t len, struct sockaddr_in *from);
static void process_rtcp_sr(const uint8_t *data, size_t len);
static void process_rtcp_bye(const uint8_t *data, size_t len);
static void process_rtcp_sdes(const uint8_t *data, size_t len);
static void send_rtcp_rr(void);
static uint64_t ntp_to_unix_ms(uint32_t ntp_sec, uint32_t ntp_frac);
static void calculate_jitter(uint32_t rtp_timestamp, int64_t arrival_time);

// Initialize RTCP receiver
esp_err_t rtcp_receiver_init(uint16_t rtp_port)
{
#ifdef CONFIG_RTCP_ENABLED
    if (!CONFIG_RTCP_ENABLED) {
        ESP_LOGI(TAG, "RTCP disabled in configuration");
        return ESP_OK;
    }
#else
    ESP_LOGI(TAG, "RTCP support enabled (CONFIG_RTCP_ENABLED not defined, using default)");
#endif
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Initializing RTCP receiver");
    ESP_LOGI(TAG, "RTP port: %d", rtp_port);
    ESP_LOGI(TAG, "RTCP port: %d", rtp_port + 1);
    ESP_LOGI(TAG, "========================================");
    
    s_state.base_rtp_port = rtp_port;
    s_state.receiver_ssrc = esp_random();
    
    // Initialize sync info
    memset(&s_state.sync_info, 0, sizeof(s_state.sync_info));
    s_state.sync_info.valid = false;
    
    // Initialize jitter calculator
    memset(&s_state.jitter_calc, 0, sizeof(s_state.jitter_calc));
    
    // Create unicast RTCP socket
    esp_err_t ret = create_rtcp_socket(rtp_port + 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RTCP socket");
        return ret;
    }
    
    ESP_LOGI(TAG, "RTCP receiver initialized successfully");
    ESP_LOGI(TAG, "  - SSRC: 0x%08X", s_state.receiver_ssrc);
    ESP_LOGI(TAG, "  - Socket FD: %d", s_state.unicast_sock);
    ESP_LOGI(TAG, "  - RR interval: %d ms", RTCP_RR_INTERVAL_MS);
    ESP_LOGI(TAG, "  - Target latency: %d ms", RTCP_TARGET_LATENCY_MS);
    return ESP_OK;
}

// Create RTCP socket
static esp_err_t create_rtcp_socket(uint16_t port)
{
    s_state.unicast_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_state.unicast_sock < 0) {
        ESP_LOGE(TAG, "Unable to create RTCP socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Set socket options
    int reuse = 1;
    setsockopt(s_state.unicast_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Set non-blocking
    int flags = fcntl(s_state.unicast_sock, F_GETFL, 0);
    fcntl(s_state.unicast_sock, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to RTCP port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    
    if (bind(s_state.unicast_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "RTCP socket unable to bind to port %d: errno %d", port, errno);
        close(s_state.unicast_sock);
        s_state.unicast_sock = -1;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "RTCP socket created and bound successfully");
    ESP_LOGI(TAG, "  - Port: %d", port);
    ESP_LOGI(TAG, "  - Socket FD: %d", s_state.unicast_sock);
    ESP_LOGI(TAG, "  - Mode: Non-blocking");
    return ESP_OK;
}

// Start RTCP receiver task
esp_err_t rtcp_receiver_start(void)
{
    if (s_state.running) {
        ESP_LOGW(TAG, "RTCP receiver already running");
        return ESP_OK;
    }
    
    s_state.running = true;
    // Increase stack size from 4096 to 8192 to prevent stack overflow
    // RTCP task has large local buffers and does network operations
    xTaskCreatePinnedToCore(rtcp_receiver_task, "rtcp_rx_task", 8192, NULL,
                           5, &s_state.task_handle, 0);
    
    ESP_LOGI(TAG, "RTCP receiver started");
    return ESP_OK;
}

// Stop RTCP receiver task
esp_err_t rtcp_receiver_stop(void)
{
    if (!s_state.running) {
        ESP_LOGW(TAG, "RTCP receiver not running");
        return ESP_OK;
    }
    
    s_state.running = false;
    
    // Wait for task to exit
    if (s_state.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_state.task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "RTCP receiver stopped");
    return ESP_OK;
}

// Clean up RTCP receiver
esp_err_t rtcp_receiver_deinit(void)
{
    // Stop task if running
    rtcp_receiver_stop();
    
    // Close sockets
    if (s_state.unicast_sock >= 0) {
        close(s_state.unicast_sock);
        s_state.unicast_sock = -1;
    }
    
    if (s_state.multicast_sock >= 0) {
        close(s_state.multicast_sock);
        s_state.multicast_sock = -1;
    }
    
    ESP_LOGI(TAG, "RTCP receiver deinitialized");
    return ESP_OK;
}

// Join multicast group for RTCP
esp_err_t rtcp_receiver_join_multicast(const char* multicast_ip, uint16_t port)
{
    if (!multicast_ip) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Joining RTCP multicast group %s:%d", multicast_ip, port + 1);
    
    // Close existing multicast socket if any
    if (s_state.multicast_sock >= 0) {
        close(s_state.multicast_sock);
        s_state.multicast_sock = -1;
    }
    
    // Create multicast socket
    s_state.multicast_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_state.multicast_sock < 0) {
        ESP_LOGE(TAG, "Unable to create RTCP multicast socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Set socket options
    int reuse = 1;
    setsockopt(s_state.multicast_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Set non-blocking
    int flags = fcntl(s_state.multicast_sock, F_GETFL, 0);
    fcntl(s_state.multicast_sock, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to RTCP port (port + 1)
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port + 1);
    
    if (bind(s_state.multicast_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "RTCP multicast socket unable to bind: errno %d", errno);
        close(s_state.multicast_sock);
        s_state.multicast_sock = -1;
        return ESP_FAIL;
    }
    
    // Join multicast group
    s_state.mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
    s_state.mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    
    if (setsockopt(s_state.multicast_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &s_state.mreq, sizeof(s_state.mreq)) < 0) {
        ESP_LOGE(TAG, "Failed to join RTCP multicast group: errno %d", errno);
        close(s_state.multicast_sock);
        s_state.multicast_sock = -1;
        return ESP_FAIL;
    }
    
    strncpy(s_state.multicast_ip, multicast_ip, sizeof(s_state.multicast_ip) - 1);
    s_state.multicast_enabled = true;
    
    ESP_LOGI(TAG, "Successfully joined RTCP multicast group");
    return ESP_OK;
}

// Leave multicast group
esp_err_t rtcp_receiver_leave_multicast(void)
{
    if (!s_state.multicast_enabled) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Leaving RTCP multicast group");
    
    if (s_state.multicast_sock >= 0) {
        setsockopt(s_state.multicast_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   &s_state.mreq, sizeof(s_state.mreq));
        close(s_state.multicast_sock);
        s_state.multicast_sock = -1;
    }
    
    s_state.multicast_enabled = false;
    memset(s_state.multicast_ip, 0, sizeof(s_state.multicast_ip));
    
    return ESP_OK;
}

// RTCP receiver task
static void rtcp_receiver_task(void *arg)
{
    // Allocate buffer on heap instead of stack to reduce stack usage
    uint8_t *buffer = malloc(RTCP_BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate RTCP buffer");
        vTaskDelete(NULL);
        return;
    }
    
    struct sockaddr_in from_addr;
    socklen_t from_len;
    fd_set read_fds;
    struct timeval tv;
    uint32_t loop_counter = 0;
    uint32_t timeout_counter = 0;
    
    ESP_LOGI(TAG, "RTCP receiver task started - listening on port %d",
             s_state.base_rtp_port + 1);
    
    while (s_state.running) {
        // Setup select
        FD_ZERO(&read_fds);
        int max_fd = -1;
        
        if (s_state.unicast_sock >= 0) {
            FD_SET(s_state.unicast_sock, &read_fds);
            max_fd = s_state.unicast_sock;
        }
        
        if (s_state.multicast_sock >= 0) {
            FD_SET(s_state.multicast_sock, &read_fds);
            if (s_state.multicast_sock > max_fd) {
                max_fd = s_state.multicast_sock;
            }
        }
        
        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            ESP_LOGE(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else if (ret == 0) {
            // Timeout - check if we need to send RR
            timeout_counter++;
            
            // Log periodic status every 50 timeouts (5 seconds)
            if (timeout_counter % 50 == 0) {
                ESP_LOGI(TAG, "RTCP task alive - Total packets: %u, SR: %u, SDES: %u, BYE: %u",
                        s_state.stats.rtcp_packets_recv,
                        s_state.stats.sr_received,
                        s_state.stats.sdes_received,
                        s_state.stats.bye_received);
                
                if (s_state.sync_info.valid) {
                    ESP_LOGI(TAG, "RTCP sync active - SSRC: 0x%08X, RTP TS: %u",
                            s_state.sync_info.ssrc, s_state.sync_info.rtp_timestamp);
                }
            }
            
            uint64_t now = esp_timer_get_time();
            if (s_state.source_addr_valid &&
                s_state.sync_info.valid &&
                (now - s_state.last_rr_time) > (RTCP_RR_INTERVAL_MS * 1000)) {
                send_rtcp_rr();
            }
            continue;
        }
        
        // Check unicast socket
        if (s_state.unicast_sock >= 0 && FD_ISSET(s_state.unicast_sock, &read_fds)) {
            from_len = sizeof(from_addr);
            int len = recvfrom(s_state.unicast_sock, buffer, RTCP_BUFFER_SIZE, 0,
                              (struct sockaddr *)&from_addr, &from_len);
            if (len > 0) {
                ESP_LOGI(TAG, "RTCP packet received on unicast: %d bytes from %s:%d",
                        len, inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
                process_rtcp_packet(buffer, len, &from_addr);
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recvfrom error on unicast: errno %d", errno);
            }
        }
        
        // Check multicast socket
        if (s_state.multicast_sock >= 0 && FD_ISSET(s_state.multicast_sock, &read_fds)) {
            from_len = sizeof(from_addr);
            int len = recvfrom(s_state.multicast_sock, buffer, RTCP_BUFFER_SIZE, 0,
                              (struct sockaddr *)&from_addr, &from_len);
            if (len > 0) {
                ESP_LOGI(TAG, "RTCP packet received on multicast: %d bytes from %s:%d",
                        len, inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
                process_rtcp_packet(buffer, len, &from_addr);
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recvfrom error on multicast: errno %d", errno);
            }
        }
        
        loop_counter++;
    }
    
    ESP_LOGI(TAG, "RTCP receiver task exiting");
    free(buffer);  // Free allocated buffer before exit
    vTaskDelete(NULL);
}

// Process RTCP packet
static void process_rtcp_packet(const uint8_t *data, size_t len, struct sockaddr_in *from)
{
    if (len < sizeof(rtcp_common_header_t)) {
        ESP_LOGW(TAG, "RTCP packet too small: %zu bytes", len);
        return;
    }
    
    rtcp_common_header_t *header = (rtcp_common_header_t *)data;
    uint8_t version = RTCP_GET_VERSION(header->vprc);
    
    if (version != RTCP_VERSION) {
        ESP_LOGW(TAG, "Invalid RTCP version: %d (expected %d), vprc=0x%02X",
                version, RTCP_VERSION, header->vprc);
        // Log first few bytes for debugging
        ESP_LOGW(TAG, "First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                data[0], data[1], data[2], data[3],
                data[4], data[5], data[6], data[7]);
        return;
    }
    
    s_state.stats.rtcp_packets_recv++;
    s_state.stats.rtcp_bytes_recv += len;
    
    ESP_LOGI(TAG, "Processing RTCP packet type %d, version %d, length %zu bytes",
            header->pt, version, len);
    
    switch (header->pt) {
        case RTCP_SR:
            ESP_LOGI(TAG, "=== RTCP Sender Report (SR) received from %s:%d ===",
                    inet_ntoa(from->sin_addr), ntohs(from->sin_port));
            process_rtcp_sr(data, len);
            s_state.stats.sr_received++;
            s_state.stats.last_sr_time = esp_timer_get_time();
            break;
            
        case RTCP_RR:
            ESP_LOGI(TAG, "RTCP Receiver Report (RR) received from %s:%d (ignored)",
                    inet_ntoa(from->sin_addr), ntohs(from->sin_port));
            break;
            
        case RTCP_SDES:
            ESP_LOGI(TAG, "RTCP Source Description (SDES) received from %s:%d",
                    inet_ntoa(from->sin_addr), ntohs(from->sin_port));
            process_rtcp_sdes(data, len);
            s_state.stats.sdes_received++;
            break;
            
        case RTCP_BYE:
            ESP_LOGI(TAG, "RTCP BYE received - sender leaving");
            process_rtcp_bye(data, len);
            s_state.stats.bye_received++;
            break;
            
        case RTCP_APP:
            ESP_LOGI(TAG, "RTCP APP packet received (type %d)", header->pt);
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown RTCP packet type: %d", header->pt);
            // Log packet header for debugging
            ESP_LOGW(TAG, "Header: vprc=0x%02X, pt=%d, length=%d",
                    header->vprc, header->pt, ntohs(header->length));
    }
}

// Process RTCP Sender Report
static void process_rtcp_sr(const uint8_t *data, size_t len)
{
    if (len < sizeof(rtcp_sr_t)) {
        ESP_LOGW(TAG, "RTCP SR too small: %zu bytes", len);
        return;
    }
    
    // Log raw bytes for debugging
    ESP_LOGI(TAG, "RTCP SR raw bytes (first 28):");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, 28, ESP_LOG_INFO);
    
    rtcp_sr_t *sr = (rtcp_sr_t *)data;
    
    // Log the raw NTP bytes before conversion
    ESP_LOGI(TAG, "Raw NTP bytes at offset 8-15: %02X %02X %02X %02X %02X %02X %02X %02X",
             data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
    
    // Extract fields
    uint32_t ssrc = ntohl(sr->ssrc);
    uint32_t ntp_sec = ntohl(sr->ntp_timestamp.seconds);
    uint32_t ntp_frac = ntohl(sr->ntp_timestamp.fraction);
    uint32_t rtp_ts = ntohl(sr->rtp_timestamp);
    uint32_t pkt_count = ntohl(sr->packet_count);
    uint32_t oct_count = ntohl(sr->octet_count);
    
    // Log the converted values
    ESP_LOGI(TAG, "Converted NTP: sec=%u (0x%08X), frac=%u (0x%08X)",
             ntp_sec, ntp_sec, ntp_frac, ntp_frac);
    
    // Convert NTP to Unix time
    uint64_t ntp_ms = ntp_to_unix_ms(ntp_sec, ntp_frac);
    
    // Update sync info
    s_state.sync_info.ssrc = ssrc;
    s_state.sync_info.ntp_timestamp_ms = ntp_ms;
    s_state.sync_info.rtp_timestamp = rtp_ts;
    s_state.sync_info.local_receive_time = esp_timer_get_time();
    s_state.sync_info.last_sr_timestamp = (ntp_sec << 16) | (ntp_frac >> 16);
    s_state.sync_info.packet_count = pkt_count;
    s_state.sync_info.octet_count = oct_count;
    s_state.sync_info.valid = true;
    
    // Calculate clock offset (for monitoring only!)
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t system_time_ms = (tv_now.tv_sec * 1000LL) + (tv_now.tv_usec / 1000);
    s_state.sync_info.clock_offset_ms = (int64_t)s_state.sync_info.ntp_timestamp_ms - (int64_t)system_time_ms;
    
#ifdef CONFIG_RTCP_LOG_SYNC_INFO
    if (CONFIG_RTCP_LOG_SYNC_INFO) {
        ESP_LOGI(TAG, "RTCP SR: SSRC=0x%08X, RTP_TS=%u, Pkts=%u, Bytes=%u",
                 ssrc, rtp_ts, pkt_count, oct_count);
        ESP_LOGI(TAG, "Clock offset: sender is %lld ms %s us (monitoring only)",
                 llabs(s_state.sync_info.clock_offset_ms),
                 s_state.sync_info.clock_offset_ms > 0 ? "ahead of" : "behind");
    } else {
#endif
        ESP_LOGD(TAG, "RTCP SR: SSRC=0x%08X, RTP_TS=%u, Pkts=%u, Bytes=%u",
                 ssrc, rtp_ts, pkt_count, oct_count);
#ifdef CONFIG_RTCP_LOG_SYNC_INFO
    }
#endif
}

// Process RTCP BYE
static void process_rtcp_bye(const uint8_t *data, size_t len)
{
    if (len < sizeof(rtcp_bye_t)) {
        return;
    }
    
    rtcp_bye_t *bye = (rtcp_bye_t *)data;
    uint32_t ssrc = ntohl(bye->ssrc);
    
    if (ssrc == s_state.sync_info.ssrc) {
        ESP_LOGI(TAG, "BYE from active source 0x%08X, clearing sync info", ssrc);
        s_state.sync_info.valid = false;
    }
}

// Process RTCP SDES (Source Description)
static void process_rtcp_sdes(const uint8_t *data, size_t len)
{
    // For now, just log that we received it
    // Could extract CNAME and other info in the future
    ESP_LOGD(TAG, "Processing RTCP SDES (not implemented)");
}

// Send RTCP Receiver Report
static void send_rtcp_rr(void)
{
#ifdef CONFIG_RTCP_SEND_RR
    if (!CONFIG_RTCP_SEND_RR) {
        return;  // RR sending disabled
    }
#endif
    
    if (!s_state.source_addr_valid || !s_state.sync_info.valid) {
        return;
    }
    
    // Build RR packet
    uint8_t rr_packet[sizeof(rtcp_rr_t) + sizeof(rtcp_report_block_t)];
    rtcp_rr_t *rr = (rtcp_rr_t *)rr_packet;
    rtcp_report_block_t *report = (rtcp_report_block_t *)(rr_packet + sizeof(rtcp_rr_t));
    
    memset(rr_packet, 0, sizeof(rr_packet));
    
    // Build header
    rr->header.vprc = RTCP_SET_VPRC(2, 0, 1); // V=2, P=0, RC=1
    rr->header.pt = RTCP_RR;
    rr->header.length = htons((sizeof(rr_packet) / 4) - 1);
    rr->ssrc = htonl(s_state.receiver_ssrc);
    
    // Build report block
    report->ssrc = htonl(s_state.sync_info.ssrc);
    
    // Calculate packet loss
    uint32_t total = s_state.rtp_packets_received + s_state.rtp_packets_lost;
    uint8_t fraction = total > 0 ? (s_state.rtp_packets_lost * 256 / total) : 0;
    report->lost_info = htonl((fraction << 24) | (s_state.rtp_packets_lost & 0xFFFFFF));
    
    // Extended highest sequence number
    uint32_t extended_seq = s_state.rtp_last_seq | ((s_state.rtp_packets_received / 65536) << 16);
    report->highest_seq_num = htonl(extended_seq);
    
    // Jitter
    report->jitter = htonl((uint32_t)s_state.jitter_calc.jitter);
    
    // Last SR timestamp and delay
    report->lsr = htonl(s_state.sync_info.last_sr_timestamp);
    uint64_t now = esp_timer_get_time();
    uint32_t delay_us = (now - s_state.sync_info.local_receive_time);
    uint32_t delay_units = (delay_us * 65536) / 1000000;
    report->dlsr = htonl(delay_units);
    
    // Send to source + 1 port
    struct sockaddr_in dest = s_state.source_addr;
    dest.sin_port = htons(ntohs(dest.sin_port) + 1);
    
    int sock = s_state.multicast_enabled ? s_state.multicast_sock : s_state.unicast_sock;
    int sent = sendto(sock, rr_packet, sizeof(rr_packet), 0, 
                     (struct sockaddr *)&dest, sizeof(dest));
    
    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to send RTCP RR: errno %d", errno);
    } else {
        ESP_LOGD(TAG, "Sent RTCP RR to %s:%d", 
                inet_ntoa(dest.sin_addr), ntohs(dest.sin_port));
        s_state.stats.rr_sent++;
    }
    
    s_state.last_rr_time = now;
    s_state.stats.last_rr_time = now;
}

// Convert NTP to Unix time
static uint64_t ntp_to_unix_ms(uint32_t ntp_sec, uint32_t ntp_frac)
{
    const uint64_t NTP_OFFSET = 2208988800ULL; // Seconds between 1900 and 1970
    
    // Debug logging to trace the conversion
    ESP_LOGI(TAG, "ntp_to_unix_ms: ntp_sec=%u (0x%08X), NTP_OFFSET=%llu",
             ntp_sec, ntp_sec, NTP_OFFSET);
    
    uint64_t unix_sec = ntp_sec - NTP_OFFSET;
    uint64_t ms = (unix_sec * 1000) + ((uint64_t)ntp_frac * 1000 / 0x100000000ULL);
    
    ESP_LOGI(TAG, "ntp_to_unix_ms: unix_sec=%llu, result_ms=%llu", unix_sec, ms);
    
    return ms;
}

// Calculate jitter
static void calculate_jitter(uint32_t rtp_timestamp, int64_t arrival_time)
{
    if (!s_state.jitter_calc.initialized) {
        s_state.jitter_calc.last_rtp_timestamp = rtp_timestamp;
        s_state.jitter_calc.last_arrival_time = arrival_time;
        s_state.jitter_calc.jitter = 0;
        s_state.jitter_calc.initialized = true;
        return;
    }
    
    // Calculate transit time difference
    int32_t transit = arrival_time - rtp_timestamp;
    int32_t last_transit = s_state.jitter_calc.last_arrival_time - s_state.jitter_calc.last_rtp_timestamp;
    int32_t d = abs(transit - last_transit);
    
    // Update jitter estimate (RFC 3550 formula)
    s_state.jitter_calc.jitter += (d - s_state.jitter_calc.jitter) / 16.0;
    
    s_state.jitter_calc.last_rtp_timestamp = rtp_timestamp;
    s_state.jitter_calc.last_arrival_time = arrival_time;
}

// Public API functions

bool rtcp_receiver_get_sync_info(rtcp_sync_info_t *sync_info)
{
    if (!sync_info || !s_state.sync_info.valid) {
        return false;
    }
    
    *sync_info = s_state.sync_info;
    return true;
}

bool rtcp_receiver_get_wall_time(uint32_t rtp_timestamp, uint64_t *wall_time_ms)
{
    if (!wall_time_ms || !s_state.sync_info.valid) {
        return false;
    }
    
    // Calculate RTP timestamp difference
    int32_t rtp_diff = (int32_t)(rtp_timestamp - s_state.sync_info.rtp_timestamp);
    
    // Convert to milliseconds (assuming 48kHz by default)
    // TODO: Make sample rate configurable
    // BUG FIX: Must cast to int64_t BEFORE multiplication to prevent overflow
    int64_t time_diff_ms = ((int64_t)rtp_diff * 1000LL) / 48000LL;
    
    // FIX: Handle negative offsets safely to prevent underflow
    if (time_diff_ms < 0) {
        uint64_t abs_diff = (uint64_t)(-time_diff_ms);
        if (abs_diff > s_state.sync_info.ntp_timestamp_ms) {
            // Would underflow - clamp to 0 and log warning
            ESP_LOGW(TAG, "Timestamp underflow prevented: ntp_ts=%llu, time_diff=%lld",
                     s_state.sync_info.ntp_timestamp_ms, time_diff_ms);
            *wall_time_ms = 0;
            return false;  // Indicate invalid timestamp
        }
        *wall_time_ms = s_state.sync_info.ntp_timestamp_ms - abs_diff;
    } else {
        // Positive offset - safe to add
        *wall_time_ms = ((uint64_t)(s_state.sync_info.local_receive_time/1000ULL) + (uint64_t)time_diff_ms);
    }
    
    return true;
}

int32_t rtcp_receiver_calculate_playout_delay(uint32_t rtp_timestamp, uint32_t sample_rate)
{
    uint64_t target_time;
    if (!rtcp_receiver_get_wall_time(rtp_timestamp, &target_time)) {
        return 0; // No sync info or invalid timestamp
    }
    
    // Adjust for actual sample rate if different from 48kHz
    if (sample_rate != 48000) {
        int32_t rtp_diff = (int32_t)(rtp_timestamp - s_state.sync_info.rtp_timestamp);
        // BUG FIX: Must cast to int64_t BEFORE multiplication to prevent overflow
        int64_t time_diff_ms = ((int64_t)rtp_diff * 1000LL) / sample_rate;
        
        // FIX: Safe calculation to prevent underflow
        if (time_diff_ms < 0) {
            uint64_t abs_diff = (uint64_t)(-time_diff_ms);
            if (abs_diff > s_state.sync_info.ntp_timestamp_ms) {
                // Very late packet
                return -RTCP_TARGET_LATENCY_MS;
            }
            target_time = s_state.sync_info.ntp_timestamp_ms - abs_diff;
        } else {
            target_time = ((uint64_t)(s_state.sync_info.local_receive_time/1000ULL) + (uint64_t)time_diff_ms);
        }
    }
    
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t current_time_ms = (tv_now.tv_sec * 1000LL) + (tv_now.tv_usec / 1000);
    
    // Calculate delay (positive = wait, negative = late)
    int32_t delay_ms = (int32_t)(target_time - current_time_ms);
    
    // Add target latency
    delay_ms += RTCP_TARGET_LATENCY_MS;
    
    return delay_ms;
}

void rtcp_receiver_update_rtp_stats(uint32_t packets_received, 
                                    uint32_t packets_lost,
                                    uint16_t last_seq,
                                    uint32_t last_rtp_timestamp,
                                    int64_t arrival_time_us)
{
    s_state.rtp_packets_received = packets_received;
    s_state.rtp_packets_lost = packets_lost;
    s_state.rtp_last_seq = last_seq;
    s_state.rtp_last_timestamp = last_rtp_timestamp;
    
    // Calculate jitter
    if (last_rtp_timestamp != 0 && arrival_time_us != 0) {
        calculate_jitter(last_rtp_timestamp, arrival_time_us);
    }
}

void rtcp_receiver_get_stats(rtcp_stats_t *stats)
{
    if (stats) {
        *stats = s_state.stats;
    }
}

void rtcp_receiver_set_source_addr(const struct sockaddr_in *addr)
{
    if (addr) {
        s_state.source_addr = *addr;
        s_state.source_addr_valid = true;
    }
}

bool rtcp_receiver_has_sync(void)
{
    return s_state.sync_info.valid;
}

int64_t rtcp_receiver_get_clock_offset(void)
{
    if (!s_state.sync_info.valid) {
        return 0;
    }
    return s_state.sync_info.clock_offset_ms;
}

uint32_t rtcp_receiver_get_jitter(void)
{
    return (uint32_t)s_state.jitter_calc.jitter;
}