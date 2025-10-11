#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Packet structure with timestamp and skip info
typedef struct packet_with_ts {
    uint8_t *packet_buffer;
    uint64_t timestamp;
    uint16_t skip_bytes;  // Number of bytes to skip from the beginning
} packet_with_ts_t;

extern bool is_underrun;
extern uint64_t received_packets;
extern uint64_t packet_buffer_size;
extern uint64_t packet_buffer_pos;
extern uint64_t target_buffer_size;

void setup_buffer();
bool push_chunk(uint8_t *chunk);
bool push_chunk_with_timestamp(uint8_t *chunk, uint64_t timestamp);
bool push_chunk_with_skip(uint8_t *chunk, uint64_t timestamp, uint16_t skip_bytes);
packet_with_ts_t *pop_chunk();  // Now returns the whole struct
void empty_buffer();

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