#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// RTCP packet types (RFC 3550)
#define RTCP_SR     200  // Sender Report
#define RTCP_RR     201  // Receiver Report
#define RTCP_SDES   202  // Source Description
#define RTCP_BYE    203  // Goodbye
#define RTCP_APP    204  // Application-defined

// RTCP version (always 2 for RFC 3550)
#define RTCP_VERSION_NUM 2

// Maximum number of simultaneous SSRCs we can track
#define RTCP_MAX_SSRC_SOURCES 4

// RTCP header structure (common to all RTCP packets)
typedef struct __attribute__((packed)) {
    uint8_t  vprc;       // Version(2), Padding(1), Reception report count(5)
    uint8_t  pt;         // Packet Type (SR=200, RR=201, etc.)
    uint16_t length;     // Packet length in 32-bit words minus 1
} rtcp_header_t;

// RTCP Sender Report (SR) packet structure
typedef struct __attribute__((packed)) {
    rtcp_header_t header;
    uint32_t ssrc;           // Synchronization source identifier
    uint32_t ntp_sec;        // NTP timestamp, seconds since 1900
    uint32_t ntp_frac;       // NTP timestamp, fractional seconds
    uint32_t rtp_timestamp;  // RTP timestamp corresponding to NTP time
    uint32_t packet_count;   // Sender's packet count
    uint32_t octet_count;    // Sender's octet count
    // Reception report blocks may follow (not used for our receiver)
} rtcp_sr_packet_t;

// RTCP Receiver Report (RR) packet structure
typedef struct __attribute__((packed)) {
    rtcp_header_t header;
    uint32_t ssrc;           // SSRC of this receiver
    // Reception report blocks follow
} rtcp_rr_packet_t;

// Reception Report Block (used in both SR and RR)
typedef struct __attribute__((packed)) {
    uint32_t ssrc;           // SSRC being reported on
    uint8_t  fraction_lost;  // Fraction of packets lost (8-bit fixed point)
    uint8_t  cumulative_lost[3]; // Cumulative packets lost (24-bit)
    uint32_t highest_seq;    // Extended highest sequence number received
    uint32_t jitter;         // Interarrival jitter
    uint32_t lsr;            // Last SR timestamp
    uint32_t dlsr;           // Delay since last SR
} rtcp_report_block_t;

 // Synchronization info storage per SSRC
 typedef struct {
     uint32_t ssrc;                    // Source SSRC
 
     // Legacy fields (kept for backward compatibility)
     uint64_t ntp_timestamp;           // Last NTP timestamp from SR (microseconds since epoch)
     uint32_t rtp_timestamp;           // Corresponding RTP timestamp
     uint64_t local_time_received;     // Local time when SR was received (esp_timer)
 
     // Preferred fields for SR anchoring (used for deterministic playout mapping)
     uint64_t last_sr_mono_us;         // Monotonic receive time of the most recent SR (esp_timer)
     uint64_t last_sr_ntp_us;          // NTP time (us) carried in the most recent SR
     uint32_t last_sr_rtp32;           // RTP timestamp from the most recent SR (32-bit) - SR anchor for NTP-anchored playout mapping

     // Linear RTP->time mapping (per-SSRC)
     double   slope_a_us_per_tick;     // nominal microseconds per RTP tick (init to 1e6 / CONFIG_SAMPLE_RATE)
     int64_t  offset_b_mono_us;        // maps sender NTP time to local monotonic: mono = ntp_us + offset_b
     uint64_t rtp_sr_base64;           // unwrapped RTP timestamp at the most recent SR
     uint64_t mono_sr_base_us;         // local monotonic time when that SR was received - SR anchor for playout mapping
     uint64_t ntp_sr_base_us;          // sender NTP time (us) carried in that SR - SR anchor for NTP-anchored playout mapping
 
     // NTP-anchored playout mapping offsets (computed at each SR)
     int64_t offset_ntp_to_receiver_us;    // receiver_ntp - sender_ntp (computed at last SR)
     int64_t wall_to_mono_offset_us;       // mono_now - receiver_ntp (computed at last SR)
 
     // RTP timestamp unwrapping state (per-SSRC)
     uint32_t unwrap_last32;           // last seen RTP 32-bit value
     uint64_t unwrap_cycles;           // number of 2^32 wraps (as a 64-bit cycle accumulator)
     uint64_t unwrap_last64;           // last produced unwrapped 64-bit value
     bool     unwrap_initialized;      // whether unwrap state has been initialized
 
     // Stats (legacy/basic)
     bool     valid;                   // Whether this entry has valid sync info
     uint32_t packet_count;            // Number of packets from this source
     uint32_t last_seq;                // Last RTP sequence number seen
     uint32_t packets_lost;            // Cumulative packets lost

     // Receiver-side RTP RX statistics (RFC 3550)
     uint32_t seq_base;                // base sequence (first seq)
     uint32_t max_seq;                 // highest seq seen (16-bit space)
     uint32_t cycles;                  // 16-bit sequence wrap cycles << 16
     uint32_t ext_max_seq;             // extended highest sequence = cycles | max_seq
     uint32_t received_pkts;           // number of packets received for this SSRC
     int32_t  cumulative_lost;         // expected - received (signed)
     double   jitter_ts;               // interarrival jitter in RTP ticks
     uint32_t transit_prev;            // previous transit (R_i - S_i) in RTP ticks
     bool     seq_initialized;         // flag for init edge cases

     // RR interval bookkeeping (for fraction-lost over last interval)
     uint32_t rr_prev_expected;        // previous "expected" at last RR
     uint32_t rr_prev_received;        // previous "received_pkts" at last RR
     uint64_t rr_prev_mono_us;         // last RR generation time (monotonic us; diagnostics)

     // Raw SR NTP fields (for LSR computation; middle 32 bits)
     uint32_t last_sr_ntp_sec;         // last SR NTP seconds (host order)
     uint32_t last_sr_ntp_frac;        // last SR NTP fraction (host order)

     // PLL accumulators (receiver-side control to stabilize playout latency)
     double   pll_offset_b_us;        // shadow of mapping offset accumulator (us)
     double   pll_slope_a_correction; // multiplicative correction on slope; 0.0 => 1.0 multiplier
     double   pll_i_err;              // integral term accumulator (us)
     uint32_t pll_obs_count;          // observations since last apply
     uint64_t pll_last_apply_mono;    // last apply time (esp_timer monotonic us)
     int32_t  pll_last_delta_b_us;    // last applied offset step (us) for diagnostics
     uint32_t rr_prev_fraction_lost;  // last computed fraction lost (0..255) for summary
 
     // Activity tracking for multi-SSRC hygiene
     uint64_t last_activity_mono_us;  // updated on SR receipt and RTP packet observe
     bool     preferred_pin;          // user/system pin to prevent eviction (default false)

     // Stable NTP→monotonic baseline for multi-speaker sync (established at RTCP lock)
     uint64_t ntp_to_mono_baseline_ntp_us;   // Receiver NTP time at baseline establishment
     uint64_t ntp_to_mono_baseline_mono_us;  // Receiver monotonic time at baseline establishment
     bool     ntp_to_mono_baseline_valid;    // Whether baseline has been established
 } rtcp_sync_info_t;

// RTCP receiver state
typedef struct {
    rtcp_sync_info_t sync_info[RTCP_MAX_SSRC_SOURCES];
    uint32_t active_sources;          // Number of active sources
    uint64_t last_rr_sent;            // Time when last RR was sent
    bool     initialized;             // Whether RTCP is initialized

    // Primary SSRC tracking (for diagnostics/policy hygiene)
    uint32_t primary_ssrc;            // Current primary SSRC selection
    bool     primary_valid;           // Whether primary_ssrc is valid
} rtcp_state_t;

// Helper macros for parsing RTCP header fields
#define RTCP_VERSION(vprc)      ((vprc) >> 6)
#define RTCP_PADDING(vprc)      (((vprc) >> 5) & 0x01)
#define RTCP_RC(vprc)           ((vprc) & 0x1F)  // Reception report count or source count

// Helper macros for NTP timestamp conversion
#define NTP_EPOCH_OFFSET        2208988800ULL  // Seconds between 1900 and 1970
#define NTP_FRAC_TO_USEC(frac)  ((uint64_t)(frac) * 1000000ULL / 0x100000000ULL)
#define USEC_TO_NTP_FRAC(usec)  ((uint32_t)((usec) * 0x100000000ULL / 1000000ULL))

// RTCP receiver API functions

/**
 * @brief Initialize RTCP receiver
 * @return ESP_OK on success
 */
esp_err_t rtcp_init(void);

/**
 * @brief Parse RTCP packet and update synchronization info
 * @param packet Raw RTCP packet data
 * @param len Length of packet in bytes
 * @return ESP_OK if packet was successfully parsed
 */
esp_err_t rtcp_parse_packet(const uint8_t *packet, size_t len);

/**
 * @brief Calculate playout time for an RTP packet
 * @param ssrc SSRC of the RTP packet
 * @param rtp_timestamp RTP timestamp from the packet
 * @param playout_time Output: calculated playout time in microseconds (esp_timer reference)
 * @return ESP_OK if timing info available, ESP_ERR_NOT_FOUND if no sync info for SSRC
 */
esp_err_t rtcp_calculate_playout_time(uint32_t ssrc, uint32_t rtp_timestamp, uint64_t *playout_time);

/**
 * @brief Generate RTCP Receiver Report (RR) with a single report block for the specified sender SSRC.
 * Builds RFC 3550-compliant RR fields: fraction lost (interval), cumulative lost, extended highest seq,
 * interarrival jitter, LSR, and DLSR.
 *
 * @param report_ssrc Sender SSRC being reported on (source of RTP we're receiving).
 * @param buffer      Output buffer to store the RR packet (network byte order).
 * @param buffer_size Size of the output buffer in bytes.
 * @param packet_size Output: exact number of bytes written into buffer.
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if SSRC not tracked/initialized; ESP_ERR_INVALID_ARG on bad args.
 */
esp_err_t rtcp_generate_rr(uint32_t report_ssrc, uint8_t *buffer, size_t buffer_size, size_t *packet_size);

/**
 * @brief Update RTP statistics for a source
 * @param ssrc Source SSRC
 * @param seq_num RTP sequence number
 */
void rtcp_update_rtp_stats(uint32_t ssrc, uint16_t seq_num);

/**
 * @brief Update receiver-side stats (jitter, extended seq, loss) on RTP packet arrival.
 * @param ssrc Source SSRC
 * @param seq  16-bit RTP sequence number (host order)
 * @param rtp_ts 32-bit RTP timestamp (host order)
 * @param arrival_rtp_ticks Arrival time converted to RTP tick units
 */
void rtcp_update_rx_stats(uint32_t ssrc, uint16_t seq, uint32_t rtp_ts, uint32_t arrival_rtp_ticks);

/**
 * @brief Get snapshot of receiver-side stats for an SSRC.
 * @return true if SSRC found; false otherwise
 */
bool rtcp_get_rx_stats(uint32_t ssrc, uint32_t *ext_max_seq, int32_t *cumulative_lost, double *jitter_ts);

/**
 * @brief Get synchronization info for a source
 * @param ssrc Source SSRC
 * @return Pointer to sync info or NULL if not found
 */
rtcp_sync_info_t* rtcp_get_sync_info(uint32_t ssrc);

/**
 * @brief Check if RTCP timing is available for a source
 * @param ssrc Source SSRC
 * @return true if sync info is available
 */
bool rtcp_has_timing_info(uint32_t ssrc);

/**
 * @brief Check if the SR-based sync mapping for the given SSRC is fresh.
 * A mapping is fresh if it has been seeded and (now_mono_us - mono_sr_base_us)
 * is less than or equal to CONFIG_RTCP_SR_MAX_AGE_MS * 1000.
 * @param ssrc Source SSRC
 * @return true if fresh; false otherwise
 */
bool rtcp_is_sync_fresh(uint32_t ssrc);
/**
 * @brief Unwrap a 32-bit RTP timestamp to 64-bit for the given SSRC (thread-safe)
 * @param ssrc SSRC of the RTP stream
 * @param rtp32 32-bit RTP timestamp
 * @param rtp64_out Output pointer to receive unwrapped 64-bit timestamp
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on bad args; ESP_ERR_NO_MEM on allocation failure
 */
esp_err_t rtcp_unwrap_rtp_timestamp(uint32_t ssrc, uint32_t rtp32, uint64_t *rtp64_out);

/**
 * @brief Observe buffer error and gently adjust RTCP mapping via a low-rate PLL.
 * @param ssrc SSRC of the stream
 * @param error_us Measured timing error in microseconds (playout_time - now - target_ahead)
 * @param sample_window_us Observation window in microseconds (e.g., nominal packet duration)
 */
void rtcp_pll_observe(uint32_t ssrc, int64_t error_us, uint32_t sample_window_us);

/**
 * @brief Primary SSRC selection and control APIs
 */
bool rtcp_get_primary_ssrc(uint32_t *ssrc_out);
void rtcp_pin_primary_ssrc(uint32_t ssrc, bool pin);
bool rtcp_consider_primary_switch(uint32_t candidate_ssrc, uint32_t *new_primary_ssrc);

/**
 * @brief Cleanup RTCP receiver
 */
void rtcp_deinit(void);

#ifdef __cplusplus
}
#endif