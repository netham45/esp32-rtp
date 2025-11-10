#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct live555_bridge_receiver live555_bridge_receiver_t;
typedef struct live555_bridge_sender live555_bridge_sender_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t payload_type;
    uint32_t local_ssrc;
    uint32_t rtcp_bandwidth_bps;
    const char *cname;
} live555_bridge_receiver_config_t;

typedef struct {
    uint32_t ext_max_seq;
    int32_t cumulative_lost;
    double jitter_ticks;
    uint32_t base_seq;
    uint32_t highest_seq;
    uint32_t last_sr_ntp_msw;
    uint32_t last_sr_ntp_lsw;
    uint64_t last_sr_wallclock_us;
    bool has_last_sr;
} live555_bridge_rx_stats_t;

/**
 * @brief Create a live555-backed receiver context.
 */
esp_err_t live555_bridge_receiver_create(const live555_bridge_receiver_config_t *config,
                                         live555_bridge_receiver_t **out_handle);

/**
 * @brief Destroy a receiver context created by live555_bridge_receiver_create().
 */
void live555_bridge_receiver_destroy(live555_bridge_receiver_t *handle);

/**
 * @brief Feed an incoming RTP packet into live555 reception stats.
 *
 * @param handle Receiver handle
 * @param ssrc   SSRC of packet (host order)
 * @param seq    RTP sequence number
 * @param rtp_ts RTP timestamp
 * @param payload_len RTP payload length (bytes)
 * @param marker Whether marker bit was set
 */
esp_err_t live555_bridge_receiver_note_rtp(live555_bridge_receiver_t *handle,
                                           uint32_t ssrc,
                                           uint16_t seq,
                                           uint32_t rtp_ts,
                                           size_t payload_len,
                                           bool marker);

/**
 * @brief Feed an incoming RTCP compound packet into live555 RTCP instance.
 */
esp_err_t live555_bridge_receiver_note_rtcp(live555_bridge_receiver_t *handle,
                                            const uint8_t *packet,
                                            size_t len);

/**
 * @brief Query the latest reception statistics for an SSRC.
 *
 * @return true if stats are available for the SSRC.
 */
bool live555_bridge_receiver_get_stats(live555_bridge_receiver_t *handle,
                                       uint32_t ssrc,
                                       live555_bridge_rx_stats_t *out_stats);

esp_err_t live555_bridge_receiver_build_rr(live555_bridge_receiver_t *handle,
                                           uint8_t *buffer,
                                           size_t buffer_size,
                                           size_t *packet_size);

typedef struct {
    uint32_t sample_rate;
    uint8_t payload_type;
    uint32_t channels;
    uint32_t rtcp_bandwidth_bps;
    const char *payload_name;
    const char *cname;
    uint32_t dest_ipv4;      // network byte order
    uint16_t dest_rtcp_port; // host order
    uint8_t ttl;
} live555_bridge_sender_config_t;

esp_err_t live555_bridge_sender_create(const live555_bridge_sender_config_t *config,
                                       live555_bridge_sender_t **out_handle);
void live555_bridge_sender_destroy(live555_bridge_sender_t *handle);

uint32_t live555_bridge_sender_ssrc(live555_bridge_sender_t *handle);
uint16_t live555_bridge_sender_initial_seq(live555_bridge_sender_t *handle);
uint32_t live555_bridge_sender_initial_timestamp(live555_bridge_sender_t *handle);

esp_err_t live555_bridge_sender_note_rtp(live555_bridge_sender_t *handle,
                                         uint32_t rtp_timestamp,
                                         size_t payload_len,
                                         uint16_t seq);

esp_err_t live555_bridge_sender_send_sr(live555_bridge_sender_t *handle);

esp_err_t live555_bridge_sender_set_destination(live555_bridge_sender_t *handle,
                                                uint32_t dest_ipv4,
                                                uint16_t dest_rtcp_port,
                                                uint8_t ttl);

/**
 * @brief Provide the current local IPv4 address (network byte order) to live555.
 */
void live555_bridge_set_ipv4(uint32_t addr_be);

#ifdef __cplusplus
}
#endif
