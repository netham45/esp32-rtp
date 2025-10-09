#ifndef RTCP_TYPES_H
#define RTCP_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// RTCP packet types (RFC 3550)
#define RTCP_SR   200  // Sender Report
#define RTCP_RR   201  // Receiver Report
#define RTCP_SDES 202  // Source Description
#define RTCP_BYE  203  // Goodbye
#define RTCP_APP  204  // Application-defined

// RTCP version
#define RTCP_VERSION 2

// RTCP common header (8 bytes total, first 4 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  vprc;      // Version(2), Padding(1), Reception report count(5)
    uint8_t  pt;        // Packet type
    uint16_t length;    // Length in 32-bit words minus 1
} rtcp_common_header_t;

// Helper macros for RTCP header fields
#define RTCP_GET_VERSION(vprc)    ((vprc) >> 6)
#define RTCP_GET_PADDING(vprc)    (((vprc) >> 5) & 0x01)
#define RTCP_GET_RC(vprc)         ((vprc) & 0x1F)
#define RTCP_SET_VPRC(v, p, rc)   (((v) << 6) | ((p) << 5) | ((rc) & 0x1F))

// NTP timestamp structure
typedef struct __attribute__((packed)) {
    uint32_t seconds;   // Seconds since 1900
    uint32_t fraction;  // Fractional seconds
} ntp_timestamp_t;

// RTCP Sender Report (SR)
typedef struct __attribute__((packed)) {
    rtcp_common_header_t header;
    uint32_t ssrc;                  // Sender's SSRC
    ntp_timestamp_t ntp_timestamp;  // NTP timestamp (8 bytes)
    uint32_t rtp_timestamp;         // Corresponding RTP timestamp
    uint32_t packet_count;          // Sender's packet count
    uint32_t octet_count;           // Sender's octet count
    // Reception report blocks may follow
} rtcp_sr_t;

// Reception Report Block
typedef struct __attribute__((packed)) {
    uint32_t ssrc;              // Source being reported
    uint32_t lost_info;         // Fraction lost (8) + cumulative lost (24)
    uint32_t highest_seq_num;   // Extended highest sequence number received
    uint32_t jitter;            // Interarrival jitter
    uint32_t lsr;               // Last SR timestamp (middle 32 bits of NTP)
    uint32_t dlsr;              // Delay since last SR
} rtcp_report_block_t;

// RTCP Receiver Report (RR)
typedef struct __attribute__((packed)) {
    rtcp_common_header_t header;
    uint32_t ssrc;              // Our SSRC as receiver
    // Report blocks follow (variable number based on RC in header)
} rtcp_rr_t;

// RTCP BYE packet
typedef struct __attribute__((packed)) {
    rtcp_common_header_t header;
    uint32_t ssrc;              // SSRC of source leaving
    // Optional reason string may follow
} rtcp_bye_t;

// RTCP SDES packet
typedef struct __attribute__((packed)) {
    rtcp_common_header_t header;
    uint32_t ssrc;              // SSRC/CSRC
    // SDES items follow
} rtcp_sdes_t;

// SDES item types
#define RTCP_SDES_END   0
#define RTCP_SDES_CNAME 1
#define RTCP_SDES_NAME  2
#define RTCP_SDES_EMAIL 3
#define RTCP_SDES_PHONE 4
#define RTCP_SDES_LOC   5
#define RTCP_SDES_TOOL  6
#define RTCP_SDES_NOTE  7
#define RTCP_SDES_PRIV  8

// Synchronization info - data extracted from RTCP SR
typedef struct {
    uint32_t ssrc;               // Source identifier
    uint64_t ntp_timestamp_ms;   // NTP time in milliseconds since Unix epoch
    uint32_t rtp_timestamp;      // Corresponding RTP timestamp
    int64_t local_receive_time;  // esp_timer_get_time() when received
    uint32_t last_sr_timestamp;  // For DLSR calculation in RR (middle 32 bits)
    uint32_t packet_count;       // Sender's packet count
    uint32_t octet_count;        // Sender's octet count
    bool valid;                  // Whether we have valid sync info
    
    // Clock offset tracking (for monitoring, not clock sync!)
    int64_t clock_offset_ms;     // Sender clock - our clock (for reference only)
} rtcp_sync_info_t;

// RTCP statistics
typedef struct {
    uint32_t sr_received;        // Number of SR packets received
    uint32_t rr_sent;           // Number of RR packets sent
    uint32_t bye_received;      // Number of BYE packets received
    uint32_t sdes_received;     // Number of SDES packets received
    uint64_t last_sr_time;      // Last time SR was received (esp_timer_get_time)
    uint64_t last_rr_time;      // Last time RR was sent
    uint32_t rtcp_packets_recv; // Total RTCP packets received
    uint32_t rtcp_bytes_recv;   // Total RTCP bytes received
} rtcp_stats_t;

// Jitter calculation state
typedef struct {
    uint32_t last_rtp_timestamp;
    int64_t last_arrival_time;
    double jitter;              // Interarrival jitter estimate
    bool initialized;
} rtcp_jitter_calc_t;

#endif // RTCP_TYPES_H