#ifndef SAP_LISTENER_H
#define SAP_LISTENER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "build_config.h"

// Configuration constants (sourced from Kconfig via build_config.h)
#define SAP_MAX_ANNOUNCEMENTS CONFIG_SAP_MAX_ANNOUNCEMENTS
#define SAP_ANNOUNCEMENT_TIMEOUT_SEC CONFIG_SAP_ANNOUNCEMENT_TIMEOUT_SEC  // 2 minutes expiration
#define SAP_CLEANUP_INTERVAL_SEC CONFIG_SAP_CLEANUP_INTERVAL_SEC          // Check for expired entries every 30 seconds
#define SAP_PORT CONFIG_SAP_PORT
#define SAP_MULTICAST_ADDR CONFIG_SAP_MULTICAST_ADDR
#define SAP_MULTICAST_ADDR_PULSEAUDIO CONFIG_SAP_PULSEAUDIO_ADDR
#define SAP_BUFFER_SIZE CONFIG_SAP_BUFFER_SIZE

/**
 * @brief SAP announcement structure with expiration tracking
 */
typedef struct {
    char stream_name[64];        // Stream/session name from SDP
    char source_ip[16];          // Source IP address of the sender
    char multicast_ip[16];       // Multicast destination IP from SDP c= line
    uint32_t sample_rate;        // Detected sample rate
    uint16_t port;               // RTP port
    time_t last_seen;            // Last time this announcement was received
    time_t first_seen;           // First time this announcement was seen
    uint32_t update_count;       // Number of times this announcement was updated
    bool active;                 // Whether this entry is active (not expired)
    char session_info[128];      // Additional session information
} sap_announcement_t;

/**
 * @brief Initialize SAP listener resources
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sap_listener_init(void);

/**
 * @brief Start the SAP listener task
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sap_listener_start(void);

/**
 * @brief Stop the SAP listener task
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sap_listener_stop(void);

/**
 * @brief Clean up SAP listener resources
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sap_listener_deinit(void);

/**
 * @brief Check if SAP listener is running
 * @return true if running, false otherwise
 */
bool sap_listener_is_running(void);

/**
 * @brief Get active (non-expired) SAP announcements
 * @param announcements Array to store announcements
 * @param max_count Maximum number of announcements to retrieve
 * @return Number of active announcements returned
 */
size_t sap_listener_get_active_announcements(sap_announcement_t* announcements, size_t max_count);

/**
 * @brief Get all SAP announcements (including expired)
 * @param count size_t that the count of announcements gets put in
 * @return Number of announcements returned
 */
const sap_announcement_t* sap_listener_get_all_announcements(size_t *count);

/**
 * @brief Get a specific announcement by stream name
 * @param stream_name Name of the stream to find
 * @param announcement Pointer to store the found announcement
 * @return true if found and active, false otherwise
 */
bool sap_listener_get_announcement_by_name(const char* stream_name, sap_announcement_t* announcement);

/**
 * @brief Clear all announcement history
 */
void sap_listener_clear_history(void);

/**
 * @brief Set the announcement expiration timeout
 * @param timeout_seconds Timeout in seconds (default: 120)
 */
void sap_listener_set_timeout(uint32_t timeout_seconds);

/**
 * @brief Get count of active announcements
 * @return Number of active announcements
 */
size_t sap_listener_get_active_count(void);

/**
 * @brief Check current stream configuration and connect/disconnect as needed
 * Should be called when SAP stream name configuration changes
 */
void sap_listener_check_stream_config(void);

#endif // SAP_LISTENER_H