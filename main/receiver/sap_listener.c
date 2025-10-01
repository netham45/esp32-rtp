#include "sap_listener.h"
#include "global.h"
#include "lifecycle_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Module state
static struct {
    int socket;
    TaskHandle_t handler_task;
    TaskHandle_t cleanup_task;
    bool is_running;
    uint32_t timeout_seconds;
    
    // Announcement tracking
    sap_announcement_t announcements[SAP_MAX_ANNOUNCEMENTS];
    size_t announcement_count;
    SemaphoreHandle_t mutex;
} s_sap_state = {
    .socket = -1,
    .handler_task = NULL,
    .cleanup_task = NULL,
    .is_running = false,
    .timeout_seconds = SAP_ANNOUNCEMENT_TIMEOUT_SEC,
    .announcement_count = 0,
    .mutex = NULL
};

// Forward declarations
static void sap_handler_task(void *pvParameters);
static void sap_cleanup_task(void *pvParameters);
static bool parse_sdp_and_get_info(const char *sdp, sap_announcement_t *announcement);
static void update_or_add_announcement(sap_announcement_t *new_announcement);
static void cleanup_expired_announcements(void);

esp_err_t sap_listener_init(void) {

    ESP_LOGI(TAG, "Initializing SAP listener");    
    // Create mutex for thread-safe access
    s_sap_state.mutex = xSemaphoreCreateMutex();
    if (s_sap_state.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SAP listener mutex");
        return ESP_FAIL;
    }
    
    // Clear announcement history
    memset(s_sap_state.announcements, 0, sizeof(s_sap_state.announcements));
    s_sap_state.announcement_count = 0;
    
    return ESP_OK;
}

esp_err_t sap_listener_start(void) {
    if (s_sap_state.is_running) {
        ESP_LOGW(TAG, "SAP listener is already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting SAP listener");
    
    // Create SAP listener socket
    struct sockaddr_in sap_addr;
    s_sap_state.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sap_state.socket < 0) {
        ESP_LOGE(TAG, "Unable to create SAP socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Set socket options
    int reuse = 1;
    setsockopt(s_sap_state.socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Bind to SAP port
    memset(&sap_addr, 0, sizeof(sap_addr));
    sap_addr.sin_family = AF_INET;
    sap_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    sap_addr.sin_port = htons(SAP_PORT);
    
    if (bind(s_sap_state.socket, (struct sockaddr *)&sap_addr, sizeof(sap_addr)) < 0) {
        ESP_LOGE(TAG, "SAP socket unable to bind: errno %d", errno);
        close(s_sap_state.socket);
        s_sap_state.socket = -1;
        return ESP_FAIL;
    }
    
    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SAP_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    
    if (setsockopt(s_sap_state.socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "Failed to join SAP multicast group");
        close(s_sap_state.socket);
        s_sap_state.socket = -1;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SAP listener started on port %d, multicast %s", SAP_PORT, SAP_MULTICAST_ADDR);
    
    // Create handler task
    BaseType_t ret = xTaskCreatePinnedToCore(
        sap_handler_task, 
        "sap_handler", 
        4096,
        NULL, 
        5, 
        &s_sap_state.handler_task, 
        0
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SAP handler task");
        close(s_sap_state.socket);
        s_sap_state.socket = -1;
        return ESP_FAIL;
    }
    
    // Create cleanup task
    ret = xTaskCreatePinnedToCore(
        sap_cleanup_task,
        "sap_cleanup",
        2048,
        NULL,
        5,
        &s_sap_state.cleanup_task,
        0
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SAP cleanup task");
        vTaskDelete(s_sap_state.handler_task);
        s_sap_state.handler_task = NULL;
        close(s_sap_state.socket);
        s_sap_state.socket = -1;
        return ESP_FAIL;
    }
    
    s_sap_state.is_running = true;
    return ESP_OK;
}

esp_err_t sap_listener_stop(void) {
    if (!s_sap_state.is_running) {
        ESP_LOGW(TAG, "SAP listener is not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping SAP listener");
    s_sap_state.is_running = false;
    
    // Stop tasks
    if (s_sap_state.handler_task) {
        vTaskDelete(s_sap_state.handler_task);
        s_sap_state.handler_task = NULL;
    }
    
    if (s_sap_state.cleanup_task) {
        vTaskDelete(s_sap_state.cleanup_task);
        s_sap_state.cleanup_task = NULL;
    }
    
    // Close socket
    if (s_sap_state.socket >= 0) {
        close(s_sap_state.socket);
        s_sap_state.socket = -1;
    }
    
    ESP_LOGI(TAG, "SAP listener stopped");
    return ESP_OK;
}

esp_err_t sap_listener_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing SAP listener");
    
    // Make sure listener is stopped
    if (s_sap_state.is_running) {
        sap_listener_stop();
    }
    
    // Delete mutex
    if (s_sap_state.mutex) {
        vSemaphoreDelete(s_sap_state.mutex);
        s_sap_state.mutex = NULL;
    }
    
    // Clear state
    memset(&s_sap_state, 0, sizeof(s_sap_state));
    s_sap_state.socket = -1;
    
    return ESP_OK;
}

bool sap_listener_is_running(void) {
    return s_sap_state.is_running;
}

size_t sap_listener_get_active_announcements(sap_announcement_t* announcements, size_t max_count) {
    if (!announcements || max_count == 0) {
        return 0;
    }
    
    // Check if module has been initialized
    if (s_sap_state.mutex == NULL) {
        ESP_LOGW(TAG, "SAP listener not initialized");
        return 0;
    }
    
    size_t count = 0;
    
    if (xSemaphoreTake(s_sap_state.mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS && count < max_count; i++) {
            if (s_sap_state.announcements[i].active) {
                memcpy(&announcements[count], &s_sap_state.announcements[i], sizeof(sap_announcement_t));
                count++;
            }
        }
        xSemaphoreGive(s_sap_state.mutex);
    }
    
    return count;
}

const sap_announcement_t* sap_listener_get_all_announcements(size_t *count) {
    *count = s_sap_state.announcement_count;
    return s_sap_state.announcements;
}

bool sap_listener_get_announcement_by_name(const char* stream_name, sap_announcement_t* announcement) {
    if (!stream_name || !announcement) {
        return false;
    }
    
    // Check if module has been initialized
    if (s_sap_state.mutex == NULL) {
        ESP_LOGW(TAG, "SAP listener not initialized");
        return false;
    }
    
    bool found = false;
    
    if (xSemaphoreTake(s_sap_state.mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS; i++) {
            if (s_sap_state.announcements[i].active &&
                strcmp(s_sap_state.announcements[i].stream_name, stream_name) == 0) {
                memcpy(announcement, &s_sap_state.announcements[i], sizeof(sap_announcement_t));
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_sap_state.mutex);
    }
    
    return found;
}

void sap_listener_clear_history(void) {
    // Check if module has been initialized
    if (s_sap_state.mutex == NULL) {
        ESP_LOGW(TAG, "SAP listener not initialized");
        return;
    }
    
    if (xSemaphoreTake(s_sap_state.mutex, portMAX_DELAY) == pdTRUE) {
        memset(s_sap_state.announcements, 0, sizeof(s_sap_state.announcements));
        s_sap_state.announcement_count = 0;
        xSemaphoreGive(s_sap_state.mutex);
    }
}

void sap_listener_set_timeout(uint32_t timeout_seconds) {
    s_sap_state.timeout_seconds = timeout_seconds;
    ESP_LOGI(TAG, "SAP announcement timeout set to %lu seconds", timeout_seconds);
}

void sap_listener_check_stream_config(void) {
    const char* configured_stream = lifecycle_get_sap_stream_name();

    // If no stream is configured, just log
    if (!configured_stream || strlen(configured_stream) == 0) {
        ESP_LOGI(TAG, "No SAP stream configured");
        return;
    }

    // Try to find configured stream in announcements
    sap_announcement_t announcement;
    if (sap_listener_get_announcement_by_name(configured_stream, &announcement)) {
        ESP_LOGI(TAG, "Found configured stream '%s', notifying lifecycle manager", configured_stream);
        
        // Use lifecycle manager to handle the stream configuration
        lifecycle_manager_notify_sap_stream(
            announcement.stream_name,
            announcement.multicast_ip,  // Use the multicast IP from SDP
            announcement.source_ip,
            announcement.port,
            announcement.sample_rate
        );
    } else {
        ESP_LOGI(TAG, "Configured stream '%s' not found in announcements", configured_stream);
    }
}

size_t sap_listener_get_active_count(void) {
    // Check if module has been initialized
    if (s_sap_state.mutex == NULL) {
        ESP_LOGW(TAG, "SAP listener not initialized");
        return 0;
    }
    
    size_t count = 0;
    
    if (xSemaphoreTake(s_sap_state.mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS; i++) {
            if (s_sap_state.announcements[i].active) {
                count++;
            }
        }
        xSemaphoreGive(s_sap_state.mutex);
    }
    
    return count;
}

// Internal functions

static void sap_handler_task(void *pvParameters) {
    char rx_buffer[SAP_BUFFER_SIZE];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    fd_set read_fds;
    struct timeval tv;

    ESP_LOGI(TAG, "SAP handler task started");

    while (s_sap_state.is_running) {
        // Setup select with timeout
        FD_ZERO(&read_fds);
        FD_SET(s_sap_state.socket, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms timeout

        int select_result = select(s_sap_state.socket + 1, &read_fds, NULL, NULL, &tv);
        
        if (select_result < 0) {
            ESP_LOGE(TAG, "SAP select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else if (select_result == 0) {
            // Timeout, no data available
            continue;
        }

        // Data is available, read it
        int len = recvfrom(s_sap_state.socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "SAP recvfrom failed: errno %d", errno);
            }
            continue;
        }

        // Parse SAP header
        if (len < 8) {
            ESP_LOGW(TAG, "SAP packet too small: %d bytes", len);
            continue;
        }

        // SAP header structure:
        // Byte 0: V(2), A(1), R(1), T(1), E(1), C(1), R(1)
        // Byte 1: Authentication length (8 bits)
        // Bytes 2-3: Message ID Hash (16 bits)
        // Bytes 4-7 (or 4-19): Originating source (32 or 128 bits based on A flag)
        // Optional: Authentication data
        // Then: SDP payload

        uint8_t *packet = (uint8_t *)rx_buffer;
        uint8_t sap_flags = packet[0];
        uint8_t auth_len = packet[1];
        
        // Check SAP version (should be 1)
        uint8_t version = (sap_flags >> 5) & 0x07;
        if (version != 1) {
            ESP_LOGW(TAG, "Unknown SAP version: %d", version);
            continue;
        }

        // Check address type (0 = IPv4, 1 = IPv6)
        uint8_t addr_type = (sap_flags >> 4) & 0x01;
        
        // Calculate SDP offset
        int sdp_offset = 4; // Basic header
        
        // Add originating source size
        if (addr_type == 0) {
            sdp_offset += 4;  // IPv4 address
        } else {
            sdp_offset += 16; // IPv6 address
        }
        
        // Add authentication data size
        sdp_offset += auth_len * 4;

        sdp_offset += 16; // 'application/sdp\n'
        
        // Check if we have enough data
        if (len <= sdp_offset) {
            ESP_LOGW(TAG, "SAP packet has no SDP payload");
            continue;
        }

        // Null-terminate SDP data
        rx_buffer[len] = 0;
        
        // Point to SDP payload
        char *sdp_data = rx_buffer + sdp_offset;
        ESP_LOGI(TAG, "Received SAP announcement from %s, SDP offset %d",
                 inet_ntoa(source_addr.sin_addr), sdp_offset);
        ESP_LOGD(TAG, "SDP content: %s", sdp_data);

        // Parse the announcement
        sap_announcement_t announcement = {0};
        
        // Store source IP in a temporary variable (parse_sdp_and_get_info will clear the struct)
        char source_ip_temp[16];
        inet_ntop(AF_INET, &source_addr.sin_addr, source_ip_temp, sizeof(source_ip_temp));
        
        // Parse SDP content
        if (parse_sdp_and_get_info(sdp_data, &announcement)) {
            // Restore the source IP after parsing (since parse_sdp_and_get_info clears the struct)
            strncpy(announcement.source_ip, source_ip_temp, sizeof(announcement.source_ip) - 1);
            announcement.source_ip[sizeof(announcement.source_ip) - 1] = '\0';
            
            // Check if this is for our device
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(netif, &ip_info);
                char my_ip_str[16];
                sprintf(my_ip_str, IPSTR, IP2STR(&ip_info.ip));

                // Update or add announcement to history
                update_or_add_announcement(&announcement);

                // If the announcement matches our IP, trigger sample rate change
                if (strcmp(announcement.source_ip, my_ip_str) == 0) {
                    ESP_LOGI(TAG, "SAP announcement matches our IP. Detected sample rate: %lu",
                            announcement.sample_rate);
                    lifecycle_manager_change_sample_rate(announcement.sample_rate);
                }

                // Check if this announcement matches configured stream name for auto-join
                const char* configured_stream = lifecycle_get_sap_stream_name();
                if (configured_stream && strlen(configured_stream) > 0) {
                    if (strcmp(announcement.stream_name, configured_stream) == 0) {
                        ESP_LOGI(TAG, "SAP announcement matches configured stream '%s'. Notifying lifecycle manager.",
                                configured_stream);
                        ESP_LOGI(TAG, "Stream details - Multicast IP: %s, Source IP: %s, Port: %d, Sample Rate: %lu",
                                announcement.multicast_ip, announcement.source_ip,
                                announcement.port, announcement.sample_rate);
                        
                        // Use lifecycle manager to handle the stream configuration
                        // The lifecycle manager will determine if we need to join multicast or configure unicast
                        lifecycle_manager_notify_sap_stream(
                            announcement.stream_name,
                            announcement.multicast_ip,  // Use the multicast IP from SDP c= line
                            announcement.source_ip,
                            announcement.port,
                            announcement.sample_rate
                        );
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Failed to parse SDP content");
        }
    }
    
    ESP_LOGI(TAG, "SAP handler task exiting");
    vTaskDelete(NULL);
}

static void sap_cleanup_task(void *pvParameters) {
    ESP_LOGI(TAG, "SAP cleanup task started");
    
    while (s_sap_state.is_running) {
        // Wait for cleanup interval
        vTaskDelay(pdMS_TO_TICKS(SAP_CLEANUP_INTERVAL_SEC * 1000));
        
        // Clean up expired announcements
        cleanup_expired_announcements();
    }
    
    ESP_LOGI(TAG, "SAP cleanup task exiting");
    vTaskDelete(NULL);
}

/**
 * @brief Safely copies the value part of a key=value SDP line.
 * * @param dest Destination buffer.
 * @param dest_size Size of the destination buffer.
 * @param line_start Pointer to the start of the line in the SDP string (e.g., at 's').
 * @param prefix_len The length of the "key=" prefix (e.g., 2 for "s=").
 */
static void copy_sdp_value(char *dest, size_t dest_size, const char *line_start, size_t prefix_len) {
    const char *value_start = line_start + prefix_len;
    const char *line_end = strchr(value_start, '\n');
    if (!line_end) {
        line_end = value_start + strlen(value_start); // Handle case where it's the last line
    }

    size_t len = line_end - value_start;

    // Trim trailing '\r' if present
    if (len > 0 && *(line_end - 1) == '\r') {
        len--;
    }

    // Ensure we don't overflow the destination buffer
    if (len >= dest_size) {
        len = dest_size - 1;
    }

    memcpy(dest, value_start, len);
    dest[len] = '\0';
}

/**
 * @brief Parses an SDP string to extract audio stream information.
 *
 * This function is corrected to properly scope attribute searches to their
 * corresponding media sections, preventing parsing errors.
 */
static bool parse_sdp_and_get_info(const char *sdp, sap_announcement_t *announcement) {
    if (!sdp || !announcement) {
        return false;
    }

    memset(announcement, 0, sizeof(sap_announcement_t));
    announcement->port = 4010; // Default port

    // Find session name (s=)
    const char *s_line = strstr(sdp, "s=");
    if (s_line) {
        copy_sdp_value(announcement->stream_name, sizeof(announcement->stream_name), s_line, 2);
    }

    // Find connection data (c=) and extract multicast IP
    const char *c_line = strstr(sdp, "c=IN IP4 ");
    if (c_line) {
        // The value starts after "c=IN IP4 "
        char ip_addr[16];
        if (sscanf(c_line + 9, "%15s", ip_addr) == 1) {
            // Store the multicast destination IP
            strncpy(announcement->multicast_ip, ip_addr, sizeof(announcement->multicast_ip) - 1);
            announcement->multicast_ip[sizeof(announcement->multicast_ip) - 1] = '\0';
            snprintf(announcement->session_info, sizeof(announcement->session_info), "Connection: %s", ip_addr);
        }
    }

    // Find the audio media description, which is our anchor
    const char *m_audio_line = strstr(sdp, "m=audio");
    if (!m_audio_line) {
        return false; // No audio media found, cannot proceed
    }

    sscanf(m_audio_line, "m=audio %hu", &announcement->port);

    // *** FIX: Isolate the audio media section ***
    // Find the start of the next media description, if any, to define our boundary.
    const char *next_m_line = strstr(m_audio_line + 1, "\nm=");
    const char *audio_section_end = next_m_line ? next_m_line : sdp + strlen(sdp);
    size_t audio_section_len = audio_section_end - m_audio_line;

    // Allocate a temporary buffer for just the audio section to search within
    char *audio_section = (char *)malloc(audio_section_len + 1);
    if (!audio_section) {
        return false; // Memory allocation failed
    }
    memcpy(audio_section, m_audio_line, audio_section_len);
    audio_section[audio_section_len] = '\0';
    
    bool found_rtpmap = false;

    // Now, perform your original searches but only within the isolated audio_section
    if (strstr(audio_section, "a=rtpmap:127 L16/48000/")) {
        announcement->sample_rate = 48000;
        found_rtpmap = true;
    } else {
        const char *rtpmap_line = strstr(audio_section, "a=rtpmap:11 L16/");
        if (rtpmap_line) {
            if (strstr(rtpmap_line, "44100")) { announcement->sample_rate = 44100; found_rtpmap = true; }
            else if (strstr(rtpmap_line, "48000")) { announcement->sample_rate = 48000; found_rtpmap = true; }
            else if (strstr(rtpmap_line, "96000")) { announcement->sample_rate = 96000; found_rtpmap = true; }
            else if (strstr(rtpmap_line, "192000")) { announcement->sample_rate = 192000; found_rtpmap = true; }
        }
    }

    free(audio_section);

    return found_rtpmap;
}

static void update_or_add_announcement(sap_announcement_t *new_announcement) {
    if (!new_announcement || strlen(new_announcement->stream_name) == 0) {
        return;
    }
    ESP_LOGI(TAG, "Adding announcement for %s", new_announcement->stream_name);
    // Check if module has been initialized
    if (s_sap_state.mutex == NULL) {
        ESP_LOGW(TAG, "SAP listener not initialized, cannot update announcements");
        return;
    }
    
    if (xSemaphoreTake(s_sap_state.mutex, portMAX_DELAY) == pdTRUE) {
        time_t current_time = time(NULL);
        new_announcement->last_seen = current_time;
        new_announcement->active = true;
        
        // Check if announcement already exists
        bool found = false;
        for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS; i++) {
            if (strcmp(s_sap_state.announcements[i].stream_name, new_announcement->stream_name) == 0) {
                // Update existing announcement
                s_sap_state.announcements[i].last_seen = current_time;
                s_sap_state.announcements[i].update_count++;
                s_sap_state.announcements[i].sample_rate = new_announcement->sample_rate;
                s_sap_state.announcements[i].port = new_announcement->port;
                s_sap_state.announcements[i].active = true;
                strncpy(s_sap_state.announcements[i].source_ip, new_announcement->source_ip,
                        sizeof(s_sap_state.announcements[i].source_ip));
                strncpy(s_sap_state.announcements[i].multicast_ip, new_announcement->multicast_ip,
                        sizeof(s_sap_state.announcements[i].multicast_ip));
                found = true;
                ESP_LOGI(TAG, "Updated SAP announcement: %s (count=%lu)",
                        new_announcement->stream_name, s_sap_state.announcements[i].update_count);
                break;
            }
        }
        
        if (!found) {
            // Find slot for new announcement
            int slot = -1;
            
            // First, try to find an expired/inactive slot
            for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS; i++) {
                if (!s_sap_state.announcements[i].active || 
                    strlen(s_sap_state.announcements[i].stream_name) == 0) {
                    slot = i;
                    break;
                }
            }
            
            // If no expired slot, find the oldest one
            if (slot == -1) {
                time_t oldest_time = current_time;
                for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS; i++) {
                    if (s_sap_state.announcements[i].last_seen < oldest_time) {
                        oldest_time = s_sap_state.announcements[i].last_seen;
                        slot = i;
                    }
                }
            }

            if (s_sap_state.announcement_count < SAP_MAX_ANNOUNCEMENTS)
                s_sap_state.announcement_count++;
            
            // Add new announcement
            if (slot >= 0) {
                new_announcement->first_seen = current_time;
                new_announcement->update_count = 1;
                memcpy(&s_sap_state.announcements[slot], new_announcement, sizeof(sap_announcement_t));
                ESP_LOGI(TAG, "Added new SAP announcement: %s at %dHz from %s (multicast: %s:%d)",
                        new_announcement->stream_name, new_announcement->sample_rate,
                        new_announcement->source_ip, new_announcement->multicast_ip,
                        new_announcement->port);
            }
        }
        
        xSemaphoreGive(s_sap_state.mutex);
    }
}

static void cleanup_expired_announcements(void) {
    // Check if module has been initialized
    if (s_sap_state.mutex == NULL) {
        ESP_LOGW(TAG, "SAP listener not initialized, cannot cleanup announcements");
        return;
    }
    
    if (xSemaphoreTake(s_sap_state.mutex, portMAX_DELAY) == pdTRUE) {
        time_t current_time = time(NULL);
        size_t expired_count = 0;
        
        for (size_t i = 0; i < SAP_MAX_ANNOUNCEMENTS; i++) {
            if (s_sap_state.announcements[i].active) {
                time_t age = current_time - s_sap_state.announcements[i].last_seen;
                if (age > s_sap_state.timeout_seconds) {
                    s_sap_state.announcements[i].active = false;
                    expired_count++;
                    ESP_LOGI(TAG, "SAP announcement expired: %s (age=%ld seconds)",
                            s_sap_state.announcements[i].stream_name, age);
                }
            }
        }
        
        if (expired_count > 0) {
            ESP_LOGI(TAG, "Marked %zu SAP announcements as expired", expired_count);
        }
        
        xSemaphoreGive(s_sap_state.mutex);
    }
}