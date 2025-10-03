// RTP packet structure: [12-byte RTP header] + [1152-byte PCM audio payload]
// Total packet size: 1164 bytes

#include "sdkconfig.h"
#include "build_config.h"
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
#include "pcm_visualizer.h"  // For pcm_viz_write

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
static TaskHandle_t udp_handler_task = NULL;

// RTP statistics
static uint32_t packets_received = 0;
static uint32_t packets_lost = 0;

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
}

static void close_udp_server(void) {
    if (unicast_sock >= 0) {
        close(unicast_sock);
        unicast_sock = -1;
    }
}

static void close_multicast_socket(void) {
    if (multicast_sock >= 0) {
        close(multicast_sock);
        multicast_sock = -1;
    }
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
        if (unicast_sock >= 0 && FD_ISSET(unicast_sock, &read_fds)) {
            active_sock = unicast_sock;
        } else if (multicast_sock >= 0 && FD_ISSET(multicast_sock, &read_fds)) {
            active_sock = multicast_sock;
        }
        
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
            // Feed PCM data to visualizer (after RTP extraction and byte order conversion)
            pcm_viz_write(audio_data, PCM_CHUNK_SIZE);
            
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
            ESP_LOGI(TAG, "RTP RX Stats: Received=%u, Lost=%u, Loss=%.2f%%, Mode=%s, vpxcc=0x%02X (V=%d), PT=%d",
                    packets_received, packets_lost, loss_rate,
                    multicast_config.enabled ? "Multicast" : "Unicast",
                    rtp->vpxcc, RTP_VERSION(rtp->vpxcc), RTP_PT(rtp->mpt));
            stats_counter = 0;
        }
    }
    
    vTaskDelete(NULL);
}

esp_err_t network_init(void) {
    ESP_LOGI(TAG, "Starting network receiver (RTP mode)");
    
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

esp_err_t network_deinit(void) {
    ESP_LOGI(TAG, "Stopping network receiver");
    
    if (udp_handler_task) {
        vTaskDelete(udp_handler_task);
        udp_handler_task = NULL;
    }
    
    close_udp_server();
    close_multicast_socket();
    
    ESP_LOGI(TAG, "Network receiver stopped");
    return ESP_OK;
}
