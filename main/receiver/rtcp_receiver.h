#ifndef RTCP_RECEIVER_H
#define RTCP_RECEIVER_H

#include "esp_err.h"
#include "rtcp_types.h"

// Forward declaration to avoid include issues
struct sockaddr_in;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize RTCP receiver module
 * @param rtp_port Base RTP port (RTCP will use rtp_port + 1)
 * @return ESP_OK on success
 */
esp_err_t rtcp_receiver_init(uint16_t rtp_port);

/**
 * Start RTCP receiver task
 * @return ESP_OK on success
 */
esp_err_t rtcp_receiver_start(void);

/**
 * Stop RTCP receiver task
 * @return ESP_OK on success
 */
esp_err_t rtcp_receiver_stop(void);

/**
 * Clean up RTCP receiver resources
 * @return ESP_OK on success
 */
esp_err_t rtcp_receiver_deinit(void);

/**
 * Handle multicast join for RTCP
 * @param multicast_ip Multicast group address
 * @param port RTP port (RTCP will use port + 1)
 * @return ESP_OK on success
 */
esp_err_t rtcp_receiver_join_multicast(const char* multicast_ip, uint16_t port);

/**
 * Handle multicast leave for RTCP
 * @return ESP_OK on success
 */
esp_err_t rtcp_receiver_leave_multicast(void);

/**
 * Get current synchronization info
 * @param sync_info Pointer to structure to fill
 * @return true if valid sync info is available
 */
bool rtcp_receiver_get_sync_info(rtcp_sync_info_t *sync_info);

/**
 * Get wall clock time for an RTP timestamp
 * @param rtp_timestamp RTP timestamp to convert
 * @param wall_time_ms Output: wall clock time in milliseconds
 * @return true if conversion successful
 */
bool rtcp_receiver_get_wall_time(uint32_t rtp_timestamp, uint64_t *wall_time_ms);

/**
 * Calculate playout delay for an RTP timestamp
 * @param rtp_timestamp RTP timestamp
 * @param sample_rate Audio sample rate (e.g., 48000)
 * @return Delay in milliseconds (positive = wait, negative = late)
 */
int32_t rtcp_receiver_calculate_playout_delay(uint32_t rtp_timestamp, uint32_t sample_rate);

/**
 * Update RTP statistics for RTCP RR generation
 * @param packets_received Total RTP packets received
 * @param packets_lost Total RTP packets lost
 * @param last_seq Last RTP sequence number
 * @param last_rtp_timestamp Last RTP timestamp received
 * @param arrival_time_us Arrival time in microseconds (esp_timer_get_time)
 */
void rtcp_receiver_update_rtp_stats(uint32_t packets_received, 
                                    uint32_t packets_lost,
                                    uint16_t last_seq,
                                    uint32_t last_rtp_timestamp,
                                    int64_t arrival_time_us);

/**
 * Get RTCP statistics
 * @param stats Pointer to structure to fill
 */
void rtcp_receiver_get_stats(rtcp_stats_t *stats);

/**
 * Set the source address for sending RTCP RR
 * @param addr Source address from last RTP packet
 */
void rtcp_receiver_set_source_addr(const struct sockaddr_in *addr);

/**
 * Check if RTCP sync is available and valid
 * @return true if we have valid synchronization info
 */
bool rtcp_receiver_has_sync(void);

/**
 * Get clock offset between sender and receiver
 * NOTE: This is for monitoring only, NOT for clock sync!
 * @return Clock offset in milliseconds (sender - receiver)
 */
int64_t rtcp_receiver_get_clock_offset(void);

/**
 * Get current jitter estimate
 * @return Jitter in RTP timestamp units
 */
uint32_t rtcp_receiver_get_jitter(void);

#ifdef __cplusplus
}
#endif

#endif // RTCP_RECEIVER_H