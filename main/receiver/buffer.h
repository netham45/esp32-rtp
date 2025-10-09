#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Buffer state variables
extern bool is_underrun;
extern uint64_t received_packets;
extern uint64_t packet_buffer_size;
extern uint64_t packet_buffer_pos;
extern uint64_t target_buffer_size;

// Metadata structure for each audio chunk (parallel to audio buffer)
typedef struct {
    uint64_t playout_time_ms;      // When to play (0 = immediate/legacy mode)
    uint32_t rtp_timestamp;        // Original RTP timestamp for debugging
    uint16_t flags;                // Bit 0: has_sync, others reserved
    uint16_t reserved;              // Padding/future use
} chunk_metadata_t;

// Pop result structure for timed playout
typedef struct {
    uint8_t *data;                 // Audio data pointer
    uint64_t playout_time_ms;      // Scheduled playout time
    bool should_play_now;          // Whether to play immediately
} pop_result_t;

// Buffer statistics structure
typedef struct {
    int64_t min_latency_ms;
    int64_t max_latency_ms;
    int64_t avg_latency_ms;
    uint32_t synced_packets;
    uint32_t unsynced_packets;
} buffer_stats_t;

// Core buffer functions
void setup_buffer();
void empty_buffer();

// Enhanced push/pop with timestamp support
bool push_chunk_with_timestamp(uint8_t *chunk, uint64_t playout_time_ms, uint32_t rtp_timestamp);
pop_result_t pop_chunk_timed(void);

// Legacy compatibility wrappers
bool push_chunk(uint8_t *chunk);
uint8_t *pop_chunk();

// Buffer statistics and monitoring
void get_buffer_stats(buffer_stats_t *stats);
bool has_enough_future_packets(void);

/**
 * @brief Update buffer growth parameters without requiring a reboot
 *
 * This function updates the buffer grow step size and max grow size
 * parameters from the lifecycle manager's current settings. It is
 * thread-safe and can be called when buffer growth configuration changes.
 *
 * @return ESP_OK on success
 */
esp_err_t buffer_update_growth_params();