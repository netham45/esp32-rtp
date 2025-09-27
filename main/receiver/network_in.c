// RTP packet structure: [12-byte RTP header] + [1152-byte PCM audio payload]
// Total packet size: 1164 bytes

#include "network_in.h"
#include "global.h"
#include "audio_out.h"
#include "spdif_out.h"
#include "lifecycle_manager.h"
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
#include "config/config_manager.h"

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
#define UDP_PORT 4010
#define MAX_RTP_PACKET_SIZE (sizeof(rtp_header_t) + (15 * 4) + 1152 + 512)  // Max: 12 + 60 (CSRCs) + 1152 (audio) + 512 (padding/extensions)
#define PCM_CHUNK_SIZE 1152  // 288 samples * 2 channels * 2 bytes (typical expected size)
#define SAP_PORT 9875
#define SAP_MULTICAST_ADDR "239.255.255.255"
#define SAP_BUFFER_SIZE 1024

// Socket and task handles
static int sock = -1;
static int sap_sock = -1;
static TaskHandle_t udp_handler_task = NULL;
static TaskHandle_t sap_handler_task = NULL;

// RTP statistics
static uint32_t packets_received = 0;
static uint32_t packets_lost = 0;

static void create_udp_server(void) {
    app_config_t* config = config_manager_get_config();
    uint16_t port = config ? config->port : UDP_PORT;
    
    struct sockaddr_in dest_addr;
    
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }
    
    // Set socket options
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to UDP port
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_port = htons(port);
    
    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        sock = -1;
        return;
    }
    
    ESP_LOGI(TAG, "UDP server listening on port %d", port);
}

static void close_udp_server(void) {
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
}

// Basic SDP parser to find sample rate and connection address
static bool parse_sdp_and_get_rate(const char *sdp, uint32_t *rate, char *ip_addr, size_t ip_addr_size) {
    // Find audio media description
    const char *a_line = strstr(sdp, "m=audio");
    if (!a_line) return false;

    // Find rtpmap attribute for L16/stereo - sender uses payload type 127
    const char *rtpmap_line = strstr(a_line, "a=rtpmap:127 L16/48000/");
    if (rtpmap_line) {
        // The sender always uses 48kHz
        *rate = 48000;
    } else {
        // Fallback to older format for compatibility
        rtpmap_line = strstr(a_line, "a=rtpmap:11 L16/");
        if (rtpmap_line) {
            if (strstr(rtpmap_line, "44100")) *rate = 44100;
            else if (strstr(rtpmap_line, "48000")) *rate = 48000;
            else if (strstr(rtpmap_line, "96000")) *rate = 96000;
            else if (strstr(rtpmap_line, "192000")) *rate = 192000;
            else return false; // Unsupported rate
        } else {
            return false;
        }
    }

    // Find connection data line
    const char *c_line = strstr(sdp, "c=IN IP4 ");
    if (c_line) {
        // Use field width limit to prevent buffer overflow
        sscanf(c_line, "c=IN IP4 %15s", ip_addr);
        ip_addr[15] = '\0';  // Ensure null termination
    } else {
        return false;
    }

    return true;
}


static void sap_handler(void *pvParameters) {
    char rx_buffer[SAP_BUFFER_SIZE];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    fd_set read_fds;
    struct timeval tv;

    while (1) {
        // Setup select with timeout
        FD_ZERO(&read_fds);
        FD_SET(sap_sock, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms timeout

        int select_result = select(sap_sock + 1, &read_fds, NULL, NULL, &tv);
        
        if (select_result < 0) {
            ESP_LOGE(TAG, "SAP select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else if (select_result == 0) {
            // Timeout, no data available
            continue;
        }

        // Data is available, read it
        int len = recvfrom(sap_sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "SAP recvfrom failed: errno %d", errno);
            }
            continue;
        }

        rx_buffer[len] = 0; // Null-terminate
        ESP_LOGD(TAG, "Received SAP announcement: %s", rx_buffer);

        uint32_t sample_rate = 0;
        char ip_addr[16] = {0};

        if (parse_sdp_and_get_rate(rx_buffer, &sample_rate, ip_addr, sizeof(ip_addr))) {
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(netif, &ip_info);
                char my_ip_str[16];
                sprintf(my_ip_str, IPSTR, IP2STR(&ip_info.ip));

                if (strcmp(ip_addr, my_ip_str) == 0) {
                    ESP_LOGI(TAG, "SAP announcement matches our IP. Detected sample rate: %lu", sample_rate);
                    lifecycle_manager_change_sample_rate(sample_rate);
                }
            }
        }
    }
    vTaskDelete(NULL);
}

static void udp_handler(void *pvParameters) {
    // RTP packet buffer - allocate enough for maximum possible RTP packet
    char rx_buffer[MAX_RTP_PACKET_SIZE];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    fd_set read_fds;
    struct timeval tv;

    while (1) {
        // Setup select with short timeout
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int select_result = select(sock + 1, &read_fds, NULL, NULL, &tv);
        
        if (select_result < 0) {
            ESP_LOGE(TAG, "select failed: errno %d", errno);
            break;
        } else if (select_result == 0) {
            // Timeout, no data available - yield to other tasks
            vTaskDelay(0);
            continue;
        }

        // Data is available, read it
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

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

        // Track sequence numbers for packet loss detection
        static uint16_t last_seq = 0;
        static bool first_packet = true;
        uint16_t seq = ntohs(rtp->seq_num);
        
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
        packets_received++;

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
        
        // Only push to audio output if we have the expected chunk size
        // This maintains compatibility with existing audio pipeline
        if (payload_len == PCM_CHUNK_SIZE) {
            push_chunk(audio_data);
        } else {
            // For non-standard sizes, we'd need to buffer and repackage
            // Log this case so we know if it happens
            static uint32_t nonstandard_counter = 0;
            if (++nonstandard_counter <= 10) {
                ESP_LOGW(TAG, "Skipping non-standard payload size %d (only first 10 warnings shown)", payload_len);
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
            ESP_LOGI(TAG, "RTP RX Stats: Received=%u, Lost=%u, Loss=%.2f%%, vpxcc=0x%02X (V=%d), PT=%d",
                    packets_received, packets_lost, loss_rate,
                    rtp->vpxcc, RTP_VERSION(rtp->vpxcc), RTP_PT(rtp->mpt));
            stats_counter = 0;
        }
    }
    
    vTaskDelete(NULL);
}

esp_err_t network_init(void) {
    ESP_LOGI(TAG, "Starting network receiver (RTP mode)");
    
    create_udp_server();

    xTaskCreatePinnedToCore(udp_handler, "udp_handler", 4096, NULL,
                           5, &udp_handler_task, 1);

    // Create SAP listener socket
    struct sockaddr_in sap_addr;
    sap_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sap_sock < 0) {
        ESP_LOGE(TAG, "Unable to create SAP socket: errno %d", errno);
    } else {
        int reuse = 1;
        setsockopt(sap_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        memset(&sap_addr, 0, sizeof(sap_addr));
        sap_addr.sin_family = AF_INET;
        sap_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        sap_addr.sin_port = htons(SAP_PORT);
        if (bind(sap_sock, (struct sockaddr *)&sap_addr, sizeof(sap_addr)) < 0) {
            ESP_LOGE(TAG, "SAP socket unable to bind: errno %d", errno);
            close(sap_sock);
            sap_sock = -1;
        } else {
            // Join multicast group
            struct ip_mreq mreq;
            mreq.imr_multiaddr.s_addr = inet_addr(SAP_MULTICAST_ADDR);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(sap_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                ESP_LOGE(TAG, "Failed to join SAP multicast group");
            } else {
                 ESP_LOGI(TAG, "SAP listener started on port %d", SAP_PORT);
                 xTaskCreatePinnedToCore(sap_handler, "sap_handler", 4096, NULL, 1, &sap_handler_task, 0);
            }
        }
    }

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
    
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket with new port %d", new_port);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Network port successfully updated to %d", new_port);
    return ESP_OK;
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

esp_err_t network_deinit(void) {
    ESP_LOGI(TAG, "Stopping network receiver");
    
    if (udp_handler_task) {
        vTaskDelete(udp_handler_task);
        udp_handler_task = NULL;
    }
    if (sap_handler_task) {
        vTaskDelete(sap_handler_task);
        sap_handler_task = NULL;
    }
    
    close_udp_server();
    if (sap_sock >= 0) {
        close(sap_sock);
        sap_sock = -1;
    }
    
    ESP_LOGI(TAG, "Network receiver stopped");
    return ESP_OK;
}
