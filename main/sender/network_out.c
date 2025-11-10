#include "sdkconfig.h"
#include "build_config.h"
#include "global.h"
#include <config.h>
#include "network_out.h"
#include "lifecycle_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>    // For htons, htonl, ntohs
#include <inttypes.h>
#include <errno.h>
#include "esp_rom_sys.h" // For ets_delay_us
#include "rom/ets_sys.h"
#include "esp_netif.h"    // For IP address functions
#include "esp_timer.h"    // For timestamp generation
#include "spdif_in.h"
#include "usb_in.h"
#include "wav_streamer.h"
#include "config/config_manager.h"  // For device_mode_t enum
#ifdef CONFIG_RTCP_ENABLED
#include "rtp/live555_bridge.h"
#define RTCP_SR_INTERVAL_MS 5000
#endif

// RTP header structure (12 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  vpxcc;      // Version(2), Padding(1), Extension(1), CSRC count(4)
    uint8_t  mpt;        // Marker(1), Payload Type(7)
    uint16_t seq_num;    // Sequence number
    uint32_t timestamp;  // RTP timestamp
    uint32_t ssrc;       // Synchronization source identifier
} rtp_header_t;


// Compile-time assertion to ensure RTP header is exactly 12 bytes
_Static_assert(sizeof(rtp_header_t) == 12, "RTP header must be exactly 12 bytes");

// SAP header structure
typedef struct __attribute__((packed)) {
    uint8_t  flags;      // V(3), A(1), R(1), T(1), E(1), C(1)
    uint8_t  auth_len;   // Authentication length (usually 0)
    uint16_t msg_id_hash;// Message ID hash
    uint32_t origin_src; // Originating source (IP address)
} sap_header_t;

// RTP constants
#define RTP_VERSION          2
#define RTP_PAYLOAD_TYPE     127  // Dynamic payload type for L16/48000/2
#define RTP_HEADER_SIZE      sizeof(rtp_header_t)  // Use struct size for consistency
// RTP timestamp increments by samples PER CHANNEL for L16 stereo
// 1152 bytes / 4 bytes per stereo sample pair = 288 stereo samples
// Each channel gets 288 samples, so timestamp increments by 288
#define RTP_TIMESTAMP_INC    288 // Samples per channel per packet at 48kHz

// SAP constants
#define SAP_MULTICAST_ADDR   CONFIG_SAP_MULTICAST_ADDR
#define SAP_PORT             CONFIG_SAP_PORT
#define SAP_ANNOUNCE_INTERVAL_MS 30000  // 30 seconds
#define SAP_VERSION          1


// Scream header for 16-bit 48KHz stereo audio
// static const char header[] = {1, 16, 2, 0, 0};  // Commented out - using RTP header instead
#define HEADER_SIZE RTP_HEADER_SIZE
#define CHUNK_SIZE PCM_CHUNK_SIZE
#define PACKET_SIZE (CHUNK_SIZE + HEADER_SIZE)

#if defined(CONFIG_USB_IN_CHUNK_SIZE)
_Static_assert(CONFIG_USB_IN_CHUNK_SIZE == PCM_CHUNK_SIZE,
               "USB PCM chunk size must match RTP chunk size");
#endif

// Socket options
#define UDP_TX_BUFFER_SIZE (PCM_CHUNK_SIZE * 4)
#define UDP_SEND_TIMEOUT_MS 100
#define MAX_SEND_RETRIES 3

// State variables
static bool s_is_sender_initialized = false;
static bool s_is_sender_running = false;
static bool s_is_muted = false;
static uint32_t s_volume = 100;
static int s_sock = -1;
static struct sockaddr_in s_dest_addr;
static TaskHandle_t s_sender_task_handle = NULL;

// RTP state variables
static uint16_t s_rtp_seq_num = 0;
static uint32_t s_rtp_timestamp = 0;
static uint32_t s_rtp_ssrc = 0;

// SAP state variables
static int s_sap_sock = -1;
static struct sockaddr_in s_sap_addr;
static char s_device_name[32] = "ESP32-Audio";
static char s_local_ip[16] = {0};
static uint64_t s_next_sap_announce_us = 0;
#ifdef CONFIG_RTCP_ENABLED
static uint64_t s_next_sr_send_us = 0;
static live555_bridge_sender_t *s_live555_sender = NULL;
#endif

// Forward declarations for multicast helper functions
static bool is_multicast_address(const char *ip_str);
static esp_err_t handle_multicast_membership(int sock, const char *multicast_ip, bool join);

static void build_rtp_packet(uint8_t *packet, const uint8_t *audio_data, size_t audio_len)
{
    // Clear the packet buffer first to avoid garbage data
    memset(packet, 0, PACKET_SIZE);
    
    // Use the RTP header struct directly for proper alignment and clarity
    rtp_header_t *header = (rtp_header_t *)packet;
    
    // Set header fields with proper network byte order conversion
    // vpxcc byte: V(2 bits)=2, P(1 bit)=0, X(1 bit)=0, CC(4 bits)=0
    // Binary: 10|0|0|0000 = 0x80
    header->vpxcc = 0x80;  // Version 2, no padding, no extension, no CSRC
    header->mpt = RTP_PAYLOAD_TYPE;  // Payload type 127 (0x7F), no marker bit
    header->seq_num = htons(s_rtp_seq_num++);  // Convert to network byte order
    header->timestamp = htonl(s_rtp_timestamp);  // Convert to network byte order
    header->ssrc = htonl(s_rtp_ssrc);  // Convert to network byte order
    
    uint8_t *payload_ptr = packet + sizeof(rtp_header_t);
    const int16_t *src_samples = (const int16_t *)audio_data;
    int16_t *dst_samples = (int16_t *)payload_ptr;
    size_t sample_count = audio_len / sizeof(int16_t);
    
    // Convert samples to network byte order (big endian) - REQUIRED for L16 per RFC 3551
    for (size_t i = 0; i < sample_count; i++) {
        dst_samples[i] = htons(src_samples[i]);
    }
    
    // Update timestamp for next packet (288 samples per packet)
    s_rtp_timestamp += RTP_TIMESTAMP_INC;
    
    // Debug logging to verify header values
    static int log_count = 0;
    if (log_count++ % 1000 == 0) {  // Log every 1000th packet to avoid spam
        ESP_LOGI(TAG, "RTP TX: vpxcc=0x%02X (V=%d), PT=%d, Seq=%u, TS=%u, SSRC=0x%08X",
                 header->vpxcc, (header->vpxcc >> 6),
                 header->mpt & 0x7F, ntohs(header->seq_num),
                 ntohl(header->timestamp), ntohl(header->ssrc));
    }
}

static int generate_sdp_message(char *sdp_buffer, size_t buffer_size)
{
    // Get current timestamp for session ID
    uint32_t session_id = (uint32_t)esp_timer_get_time();
    
    // Get local IP address
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to get network interface");
        return -1;
    }
    esp_netif_get_ip_info(netif, &ip_info);
    snprintf(s_local_ip, sizeof(s_local_ip), IPSTR, IP2STR(&ip_info.ip));
    
    // Get destination from lifecycle manager
    const char* dest_ip = lifecycle_get_sender_destination_ip();
    uint16_t dest_port = lifecycle_get_sender_destination_port();
    
    // Generate SDP
    int len = snprintf(sdp_buffer, buffer_size,
        "v=0\r\n"
        "o=- %lu %lu IN IP4 %s\r\n"
        "s=%s\r\n"
        "i=48kHz 16-bit Stereo Audio from %s\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "a=recvonly\r\n"
        "m=audio %u RTP/AVP %d\r\n"
        "a=rtpmap:%d L16/48000/2\r\n"
        "a=ptime:6\r\n",
        session_id, session_id, s_local_ip,
        s_device_name,
        s_device_name,
        dest_ip,
        dest_port,
        RTP_PAYLOAD_TYPE,
        RTP_PAYLOAD_TYPE
    );
    
    return len;
}
static void sap_announcement_tick(void)
{
    if (!s_is_sender_running || s_sap_sock < 0) {
        return;
    }

    uint64_t now_us = esp_timer_get_time();
    if (s_next_sap_announce_us != 0 && now_us < s_next_sap_announce_us) {
        return;
    }

    char sdp_buffer[512];
    uint8_t sap_packet[600];
    sap_header_t *sap_header = (sap_header_t *)sap_packet;

    // Prepare SAP header for this announcement
    sap_header->flags = 0x20;  // V=1, no authentication, IPv4
    sap_header->auth_len = 0;
    sap_header->msg_id_hash = htons((uint16_t)(s_rtp_ssrc & 0xFFFF));

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_get_ip_info(netif, &ip_info);
        sap_header->origin_src = ip_info.ip.addr;
    } else {
        sap_header->origin_src = inet_addr(s_local_ip);
    }

    // Generate SDP content
    int sdp_len = generate_sdp_message(sdp_buffer, sizeof(sdp_buffer));
    if (sdp_len < 0 || sdp_len >= (int)sizeof(sdp_buffer)) {
        ESP_LOGE(TAG, "Failed to generate SDP message or message too large");
        s_next_sap_announce_us = now_us + 5000000ULL;
        return;
    }

    const char *mime_type = "application/sdp";
    size_t mime_len = strlen(mime_type);

    // Calculate required size and validate bounds before copying
    size_t required_size = sizeof(sap_header_t) + mime_len + 1 + (size_t)sdp_len;
    if (required_size > sizeof(sap_packet)) {
        ESP_LOGE(TAG, "SAP packet too large (%zu bytes > %zu), skipping",
                 required_size, sizeof(sap_packet));
        s_next_sap_announce_us = now_us + 5000000ULL;
        return;
    }

    char *payload_ptr = (char *)(sap_packet + sizeof(sap_header_t));
    size_t remaining_space = sizeof(sap_packet) - sizeof(sap_header_t);

    // Copy MIME type
    if (mime_len >= remaining_space) {
        ESP_LOGE(TAG, "MIME type too long, skipping");
        s_next_sap_announce_us = now_us + 5000000ULL;
        return;
    }
    memcpy(payload_ptr, mime_type, mime_len);
    payload_ptr[mime_len] = '\0';
    remaining_space -= (mime_len + 1);

    // Copy SDP payload
    if ((size_t)sdp_len > remaining_space) {
        ESP_LOGE(TAG, "SDP content too long, skipping");
        s_next_sap_announce_us = now_us + 5000000ULL;
        return;
    }
    memcpy(payload_ptr + mime_len + 1, sdp_buffer, (size_t)sdp_len);

    size_t packet_size = required_size;

    int sent = sendto(s_sap_sock, sap_packet, packet_size, 0,
                      (struct sockaddr *)&s_sap_addr, sizeof(s_sap_addr));

    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to send SAP announcement: errno %d", errno);
        s_next_sap_announce_us = now_us + 5000000ULL;
        return;
    }

    ESP_LOGD(TAG, "Sent SAP announcement (%d bytes)", sent);
    s_next_sap_announce_us = now_us + (uint64_t)SAP_ANNOUNCE_INTERVAL_MS * 1000ULL;
}


static void rtp_sender_task(void *arg);

esp_err_t rtp_sender_init(void)
{
    if (s_is_sender_initialized) {
        ESP_LOGW(TAG, "RTP sender already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing RTP sender");
    
    // Initialize the socket
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Configure socket options for better reliability
    
    // Set timeout to prevent blocking too long on send
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = UDP_SEND_TIMEOUT_MS * 1000;
    if (setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_SNDTIMEO: errno %d", errno);
    }
    
    // Set QoS priority for audio traffic
    #if defined(IP_TOS) && defined(IPTOS_DSCP_EF)
    // Use Expedited Forwarding (EF) for real-time audio
    int opt_val = IPTOS_DSCP_EF;
    if (setsockopt(s_sock, IPPROTO_IP, IP_TOS, &opt_val, sizeof(opt_val)) < 0) {
        ESP_LOGW(TAG, "Failed to set IP_TOS: errno %d", errno);
    }
    #endif
    
    // Initialize SAP socket for announcements
    ESP_LOGI(TAG, "Initializing SAP socket for announcements");
    s_sap_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sap_sock < 0) {
        ESP_LOGE(TAG, "Unable to create SAP socket: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    
    // Configure SAP multicast address
    memset(&s_sap_addr, 0, sizeof(s_sap_addr));
    s_sap_addr.sin_family = AF_INET;
    s_sap_addr.sin_addr.s_addr = inet_addr(SAP_MULTICAST_ADDR);
    s_sap_addr.sin_port = htons(SAP_PORT);
    
    // Set multicast TTL for SAP
    uint8_t ttl = 15;  // Reasonable TTL for local network
    if (setsockopt(s_sap_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        ESP_LOGW(TAG, "Failed to set SAP multicast TTL: errno %d", errno);
    }
    
    ESP_LOGI(TAG, "SAP socket configured for %s:%d", SAP_MULTICAST_ADDR, SAP_PORT);
    
    // Initialize the destination address from lifecycle manager
    const char* dest_ip = lifecycle_get_sender_destination_ip();
    uint16_t dest_port = lifecycle_get_sender_destination_port();
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_addr.s_addr = inet_addr(dest_ip);
    s_dest_addr.sin_port = htons(dest_port);
    
    // Check if destination is multicast and join the group if needed
    bool dest_is_multicast = is_multicast_address(dest_ip);
    if (dest_is_multicast) {
        ESP_LOGI(TAG, "Detected multicast destination %s, joining multicast group", dest_ip);
        handle_multicast_membership(s_sock, dest_ip, true);
    }

#ifdef CONFIG_RTCP_ENABLED
    uint16_t rtcp_port = (dest_port == UINT16_MAX) ? dest_port : (uint16_t)(dest_port + 1U);

    live555_bridge_sender_config_t bridge_cfg = {
        .sample_rate = lifecycle_get_sample_rate(),
        .payload_type = RTP_PAYLOAD_TYPE,
        .channels = 2,
        .rtcp_bandwidth_bps = 0,
        .payload_name = "L16",
        .cname = s_device_name,
        .dest_ipv4 = s_dest_addr.sin_addr.s_addr,
        .dest_rtcp_port = rtcp_port,
        .ttl = 1,
    };

    esp_err_t bridge_rc = live555_bridge_sender_create(&bridge_cfg, &s_live555_sender);
    if (bridge_rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize live555 sender bridge: %s", esp_err_to_name(bridge_rc));
        if (dest_is_multicast) {
            handle_multicast_membership(s_sock, dest_ip, false);
        }
        close(s_sock);
        s_sock = -1;
        close(s_sap_sock);
        s_sap_sock = -1;
        return bridge_rc;
    }

    s_rtp_ssrc = live555_bridge_sender_ssrc(s_live555_sender);
    s_rtp_seq_num = live555_bridge_sender_initial_seq(s_live555_sender);
    s_rtp_timestamp = live555_bridge_sender_initial_timestamp(s_live555_sender);

    ESP_LOGI(TAG, "RTP SSRC: 0x%08X, Initial seq: %u ts=%u",
             s_rtp_ssrc, s_rtp_seq_num, s_rtp_timestamp);
#else
    s_rtp_ssrc = esp_random();
    s_rtp_seq_num = esp_random() & 0xFFFF;
    s_rtp_timestamp = 0;
    ESP_LOGI(TAG, "RTP SSRC: 0x%08X, Initial seq: %u", s_rtp_ssrc, s_rtp_seq_num);
#endif

    s_is_sender_initialized = true;

    uint32_t sample_rate = lifecycle_get_sample_rate();
    uint8_t bit_depth = lifecycle_get_bit_depth();
    uint8_t channels = 2;

    esp_err_t wav_err = wav_streamer_init(sample_rate, bit_depth, channels);
    if (wav_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize WAV streamer: 0x%x", wav_err);
    }

    return ESP_OK;
}

esp_err_t rtp_sender_start(void)
{
    if (!s_is_sender_initialized) {
        ESP_LOGE(TAG, "RTP sender not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_is_sender_running) {
        ESP_LOGW(TAG, "RTP sender already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting RTP sender");
    
    s_is_sender_running = true;
    s_next_sap_announce_us = 0;
#ifdef CONFIG_RTCP_ENABLED
    if (s_live555_sender) {
        s_next_sr_send_us = esp_timer_get_time() + ((uint64_t)RTCP_SR_INTERVAL_MS * 1000ULL);
    } else {
        s_next_sr_send_us = 0;
    }
#endif

    // Create the sender task
    xTaskCreatePinnedToCore(rtp_sender_task, "rtp_sender_task", 8192, NULL, 7, &s_sender_task_handle, 1);

    ESP_LOGI(TAG, "SAP announcements scheduled via lifecycle tick");
    
    return ESP_OK;
}

esp_err_t rtp_sender_stop(void)
{
    if (!s_is_sender_initialized) {
        ESP_LOGE(TAG, "RTP sender not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_is_sender_running) {
        ESP_LOGW(TAG, "RTP sender not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping RTP sender");
    
    s_is_sender_running = false;
#ifdef CONFIG_RTCP_ENABLED
    s_next_sr_send_us = 0;
    if (s_live555_sender) {
        (void)live555_bridge_sender_send_sr(s_live555_sender);
        live555_bridge_sender_destroy(s_live555_sender);
        s_live555_sender = NULL;
    }
#endif

    // Wait for tasks to self-delete (they both check s_is_sender_running and call vTaskDelete(NULL))
    // Give them time to clean up properly
    if (s_sender_task_handle) {
        ESP_LOGI(TAG, "Waiting for sender task to finish...");
        vTaskDelay(pdMS_TO_TICKS(100)); // Give tasks time to exit cleanly
        
        // Clear handles since tasks self-delete
        s_sender_task_handle = NULL;
    }

    // Clean up sockets
    if (s_sock != -1) {
        close(s_sock);
        s_sock = -1;
    }
    
    if (s_sap_sock != -1) {
        close(s_sap_sock);
        s_sap_sock = -1;
    }

    s_next_sap_announce_us = 0;
    wav_streamer_deinit();

    return ESP_OK;
}

bool rtp_sender_is_running(void)
{
    return s_is_sender_running;
}

void rtp_sender_tick(void)
{
    sap_announcement_tick();
}

void rtp_sender_set_mute(bool mute)
{
    s_is_muted = mute;
    ESP_LOGI(TAG, "RTP sender mute set to %d", mute);
}

void rtp_sender_set_volume(uint32_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    
    s_volume = volume;
    ESP_LOGI(TAG, "RTP sender volume set to %"PRIu32"", volume);
}

esp_err_t rtp_sender_update_destination(void)
{
    if (!s_is_sender_initialized) {
        ESP_LOGE(TAG, "RTP sender not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Store previous destination to check if we need to leave a multicast group
    static char prev_dest_ip[16] = {0};
    bool had_multicast = (strlen(prev_dest_ip) > 0 && is_multicast_address(prev_dest_ip));
    
    // Get the destination address from lifecycle manager
    const char* dest_ip = lifecycle_get_sender_destination_ip();
    uint16_t dest_port = lifecycle_get_sender_destination_port();
    
    // Validate IP address
    if (dest_ip == NULL || strlen(dest_ip) == 0) {
        ESP_LOGW(TAG, "Destination IP is empty or NULL, using default multicast address");
        dest_ip = "239.255.77.77";  // Default multicast address for Scream
    }
    
    // Validate port
    if (dest_port == 0) {
        ESP_LOGW(TAG, "Destination port is 0, using default port %d", CONFIG_RTP_PORT);
        dest_port = CONFIG_RTP_PORT;
    }
    
    // Validate IP format by attempting to convert it
    struct in_addr addr_test;
    if (inet_aton(dest_ip, &addr_test) == 0) {
        ESP_LOGE(TAG, "Invalid IP address format: %s", dest_ip);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Update the destination address structure
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_addr = addr_test;  // Use the validated address
    s_dest_addr.sin_port = htons(dest_port);
    
    // Handle multicast group membership changes
    bool is_multicast = is_multicast_address(dest_ip);
    
    // Leave previous multicast group if it was different
    if (had_multicast && (!is_multicast || strcmp(prev_dest_ip, dest_ip) != 0)) {
        ESP_LOGI(TAG, "Leaving previous multicast group %s", prev_dest_ip);
        esp_err_t err = handle_multicast_membership(s_sock, prev_dest_ip, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to leave multicast group %s, continuing anyway", prev_dest_ip);
        }
    }
    
    // Join new multicast group if needed
    if (is_multicast && (!had_multicast || strcmp(prev_dest_ip, dest_ip) != 0)) {
        ESP_LOGI(TAG, "Joining new multicast group %s", dest_ip);
        esp_err_t err = handle_multicast_membership(s_sock, dest_ip, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to join multicast group %s", dest_ip);
            // Don't fail completely, unicast might still work
            if (!had_multicast) {
                // But if we're switching from unicast to multicast and can't join, that's an error
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

#ifdef CONFIG_RTCP_ENABLED
    if (s_live555_sender) {
        uint8_t ttl = is_multicast ? 15 : 1;
        uint16_t rtcp_port = (dest_port == UINT16_MAX) ? dest_port : (uint16_t)(dest_port + 1U);
        esp_err_t bridge_rc = live555_bridge_sender_set_destination(s_live555_sender,
                                                                    s_dest_addr.sin_addr.s_addr,
                                                                    rtcp_port,
                                                                    ttl);
        if (bridge_rc != ESP_OK) {
            ESP_LOGW(TAG, "Failed to update live555 RTCP destination: %s", esp_err_to_name(bridge_rc));
        }
    }
#endif
    
    // Save current destination for next update
    strncpy(prev_dest_ip, dest_ip, sizeof(prev_dest_ip) - 1);
    prev_dest_ip[sizeof(prev_dest_ip) - 1] = '\0';
    
    ESP_LOGI(TAG, "Updated RTP sender destination to %s:%u%s",
             dest_ip, dest_port, is_multicast ? " (multicast)" : "");
    
    // If sender is running, the changes take effect immediately on the next packet
    if (s_is_sender_running) {
        ESP_LOGI(TAG, "Sender is running, destination changes will take effect immediately");
    }
    
    return ESP_OK;
}

// Helper function to check if an IP address is multicast
static bool is_multicast_address(const char *ip_str)
{
    struct in_addr addr;
    if (inet_aton(ip_str, &addr) == 0) {
        return false;
    }
    
    // Multicast addresses are in the range 224.0.0.0 to 239.255.255.255
    uint32_t ip = ntohl(addr.s_addr);
    return (ip >= 0xE0000000 && ip <= 0xEFFFFFFF);
}

// Helper function to join or leave multicast group
static esp_err_t handle_multicast_membership(int sock, const char *multicast_ip, bool join)
{
    struct ip_mreq mreq;
    
    // Set the multicast group address
    mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
    
    // Use any available interface
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    
    // Join or leave the multicast group
    int opt = join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
    if (setsockopt(sock, IPPROTO_IP, opt, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "Failed to %s multicast group %s: errno %d",
                 join ? "join" : "leave", multicast_ip, errno);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Successfully %s multicast group %s",
             join ? "joined" : "left", multicast_ip);
    
    // If joining, also set multicast TTL
    if (join) {
        uint8_t ttl = 64;  // Default TTL for multicast
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            ESP_LOGW(TAG, "Failed to set multicast TTL: errno %d", errno);
        }
    }
    
    return ESP_OK;
}

static void rtp_sender_task(void *arg)
{
    static unsigned char rtp_packet[PACKET_SIZE];
    char audio_buffer[CHUNK_SIZE];
    size_t bytes_in_buffer = 0;
    struct {
        size_t len;
        size_t offset;
        uint8_t data[CHUNK_SIZE];
    } ring_stash = {0};

    // For pacing the sender to match the audio rate
    // 1152 bytes = 288 stereo samples = 288/48000 = 6ms exactly
    // Using esp_timer for more precise timing to avoid drift
    int64_t next_send_time_us = esp_timer_get_time();
    const int64_t send_interval_us = 6000; // 6ms in microseconds

    RingbufHandle_t pcm_out_buffer = NULL;

    device_mode_t current_mode = lifecycle_get_device_mode();
    ESP_LOGI(TAG, "Waiting for ringbuf, mode: %d", current_mode);
    // Both USB and SPDIF sender modes use the same ring buffer approach
    if (!pcm_out_buffer) {

        if (current_mode == MODE_SENDER_SPDIF) {
            while (spdif_in_get_ringbuf() == NULL) vTaskDelay(0);
            pcm_out_buffer = spdif_in_get_ringbuf();
        } else  if (current_mode == MODE_SENDER_USB){
            while (usb_in_get_ringbuf() == NULL) vTaskDelay(0);
            pcm_out_buffer = usb_in_get_ringbuf();
        }
    }

    ESP_LOGI(TAG, "Got ringbuf, mode: %d", current_mode);
    
    while (s_is_sender_running) {
        if (s_is_muted) {
            vTaskDelay(pdMS_TO_TICKS(100));
            bytes_in_buffer = 0; // Reset buffer when muted
            ring_stash.len = 0;
            ring_stash.offset = 0;
            continue;
        }

        // We need to fill the buffer completely before sending
        bool ringbuf_empty = false;
        while (bytes_in_buffer < CHUNK_SIZE) {
            size_t remaining = CHUNK_SIZE - bytes_in_buffer;

            if (ring_stash.len > ring_stash.offset) {
                size_t available = ring_stash.len - ring_stash.offset;
                size_t to_copy = available < remaining ? available : remaining;
                memcpy((uint8_t *)audio_buffer + bytes_in_buffer,
                       ring_stash.data + ring_stash.offset, to_copy);
                ring_stash.offset += to_copy;
                bytes_in_buffer += to_copy;

                if (ring_stash.offset == ring_stash.len) {
                    ring_stash.len = 0;
                    ring_stash.offset = 0;
                }

                continue;
            }

            size_t item_size = 0;
            // Try to receive up to a full chunk from the ring buffer
            // Increased timeout to avoid missing data
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(
                pcm_out_buffer,  // Ring buffer populated by either usb_in or spdif_in
                &item_size,
                pdMS_TO_TICKS(10),  // Allow entire USB chunk (≈6ms) to arrive
                CHUNK_SIZE
            );
            if (item == NULL) {
                // No data yet; re-evaluate and let the pacing logic decide
                vTaskDelay(pdMS_TO_TICKS(1));
                ringbuf_empty = true;
                next_send_time_us = esp_timer_get_time();
                break;
            }

            if (item_size == 0) {
                vRingbufferReturnItem(pcm_out_buffer, (void *)item);
                continue;
            }

            size_t to_copy = item_size < remaining ? item_size : remaining;
            memcpy((uint8_t *)audio_buffer + bytes_in_buffer, item, to_copy);
            bytes_in_buffer += to_copy;

            size_t remainder = item_size - to_copy;
            if (remainder > 0) {
                if (remainder > sizeof(ring_stash.data)) {
                    ESP_LOGE(TAG, "PCM stash overflow: remainder=%zu, stash=%zu",
                             remainder, sizeof(ring_stash.data));
                    remainder = sizeof(ring_stash.data);
                }
                memcpy(ring_stash.data, item + to_copy, remainder);
                ring_stash.len = remainder;
                ring_stash.offset = 0;
            }

            vRingbufferReturnItem(pcm_out_buffer, (void *)item);
        }

        if (ringbuf_empty) {
            continue;
        }

        // If we have a full chunk, send it
        if (bytes_in_buffer == CHUNK_SIZE) {
            // Debug: Check for potential data corruption and timing every 1000 packets
            static int packet_count = 0;
            static int64_t last_packet_time_us = 0;
            packet_count++;
            
            if (packet_count % 1000 == 0) {
                int16_t *debug_samples = (int16_t*)audio_buffer;
                int zero_count = 0;
                int16_t max_val = 0;
                int16_t min_val = 0;
                
                // Check for data patterns
                for (int i = 0; i < CHUNK_SIZE / 2; i++) {
                    if (debug_samples[i] == 0) zero_count++;
                    if (debug_samples[i] > max_val) max_val = debug_samples[i];
                    if (debug_samples[i] < min_val) min_val = debug_samples[i];
                }
                
                // Check timing
                int64_t now_us = esp_timer_get_time();
                int64_t delta_us = last_packet_time_us ? (now_us - last_packet_time_us) : 0;
                last_packet_time_us = now_us;
                
                // Also check first few samples for continuity
                ESP_LOGD(TAG, "Audio stats: zeros=%d/%d, range=[%d,%d], packets=%d, interval=%lldms",
                         zero_count, CHUNK_SIZE/2, min_val, max_val, packet_count, delta_us/1000);
                ESP_LOGD(TAG, "First samples: [%d, %d, %d, %d]",
                         debug_samples[0], debug_samples[1], debug_samples[2], debug_samples[3]);
            }
            
            // Apply volume
            float volume = lifecycle_get_volume();
            if (volume < 1.0f) {
                int16_t *samples = (int16_t*)audio_buffer;
                int num_samples = CHUNK_SIZE / 2;
                for (int i = 0; i < num_samples; i++) {
                    samples[i] = (int16_t)(samples[i] * volume);
                }
            }

            // Feed PCM data to visualizer (after volume adjustment, before RTP packet construction)
            //pcm_viz_write((const uint8_t*)audio_buffer, CHUNK_SIZE);

            if (wav_streamer_is_ready()) {
                wav_streamer_push((const uint8_t*)audio_buffer, CHUNK_SIZE);
            }

            // Build RTP packet
#ifdef CONFIG_RTCP_ENABLED
            uint16_t seq_before = s_rtp_seq_num;
            uint32_t ts_before = s_rtp_timestamp;
#endif
            build_rtp_packet(rtp_packet, (uint8_t*)audio_buffer, CHUNK_SIZE);

#ifdef CONFIG_RTCP_ENABLED
            if (s_live555_sender) {
                esp_err_t note_rc = live555_bridge_sender_note_rtp(s_live555_sender,
                                                                   ts_before,
                                                                   CHUNK_SIZE,
                                                                   seq_before);
                if (note_rc != ESP_OK) {
                    ESP_LOGW(TAG, "live555 RTP stat update failed: %s", esp_err_to_name(note_rc));
                }
                uint64_t now_sr = esp_timer_get_time();
                if (s_next_sr_send_us != 0 && now_sr >= s_next_sr_send_us) {
                    esp_err_t sr_rc = live555_bridge_sender_send_sr(s_live555_sender);
                    if (sr_rc != ESP_OK) {
                        ESP_LOGW(TAG, "live555 SR send failed: %s", esp_err_to_name(sr_rc));
                    }
                    s_next_sr_send_us = now_sr + ((uint64_t)RTCP_SR_INTERVAL_MS * 1000ULL);
                }
            }
#endif

            int sent = -1;
            int retry_count = 0;
            while (sent < 0 && retry_count < MAX_SEND_RETRIES) {
                sent = sendto(s_sock, rtp_packet, PACKET_SIZE, 0,
                             (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
                vTaskDelay(0);
                if (sent > 0) {
                    //ESP_LOGI(TAG, "Sent %d bytes to network", sent);
                } else {
                    ESP_LOGW(TAG, "Failed to send UDP packet: errno %d, retry %d", errno, retry_count + 1);
                    vTaskDelay(retry_count * 100);
                    retry_count++;
                }
            }

            // Reset buffer for next chunk
            bytes_in_buffer = 0;
            
            // Pace the sender using precise timing to avoid drift
            // Calculate next send time
            next_send_time_us += send_interval_us;
            int64_t now_us = esp_timer_get_time();
            int64_t delay_us = next_send_time_us - now_us;
            
            // Only delay if we're not behind schedule
            if (delay_us > 0) {
                // Use busy wait for very short delays to maintain precision
                if (delay_us < 1000) {
                    // Busy wait for sub-millisecond precision
            while (esp_timer_get_time() < next_send_time_us) {
                __asm__ __volatile__("nop");
            }
        } else {
                    // Use vTaskDelay for longer waits
                    vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
                }
            } else if (delay_us < -send_interval_us) {
                // We're more than one interval behind, reset timing
                ESP_LOGW(TAG, "RTP sender lagging by %lld us, resetting timing", -delay_us);
                next_send_time_us = esp_timer_get_time();
            }
        }
    }
    
    ESP_LOGI(TAG, "RTP sender task exiting, deleting task");
    vTaskDelete(NULL);
}
