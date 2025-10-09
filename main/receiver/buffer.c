#include "esp_psram.h"
#include "global.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include <sys/time.h>  // For gettimeofday()
#include "lifecycle_manager.h"
#include "buffer.h"
#include "sdkconfig.h"

// Use configuration values from Kconfig, with fallbacks
#ifndef CONFIG_MIN_FUTURE_BUFFER_MS
#define CONFIG_MIN_FUTURE_BUFFER_MS 50    // Fallback if not configured
#endif

#ifndef CONFIG_MAX_PLAYOUT_DELAY_MS
#define CONFIG_MAX_PLAYOUT_DELAY_MS 100   // Fallback if not configured
#endif

// Flag if the stream is currently underrun and rebuffering
bool is_underrun                            = true;
// Number of received packets since last underflow
uint64_t received_packets               = 0;
// Number of packets in ring buffer
uint64_t packet_buffer_size             = 0;
// Position of ring buffer read head
uint64_t packet_buffer_pos              = 0;
// Number of bytes to buffer
uint64_t target_buffer_size             = 0;
// Buffer of packets to send
uint8_t **packet_buffer = NULL;
// Parallel metadata buffer for timestamps
chunk_metadata_t *packet_metadata = NULL;
portMUX_TYPE buffer_mutex = portMUX_INITIALIZER_UNLOCKED;

// Cached buffer growth parameters for thread-safe access
static uint8_t buffer_grow_step_size = 0;
static uint8_t buffer_max_grow_size = 0;

// Forward declaration for internal function
static bool has_enough_future_packets_with_time(uint64_t now_ms);

void set_underrun() {
  if (!is_underrun) {
    received_packets = 0;
    
    // CRITICAL FIX: Ensure buffer_grow_step_size and buffer_max_grow_size are valid
    if (buffer_grow_step_size == 0 || buffer_max_grow_size == 0) {
        // Not initialized yet or corrupted - use safe defaults
        buffer_grow_step_size = 2;
        buffer_max_grow_size = 20;
    }
    
    target_buffer_size += buffer_grow_step_size;
    if (target_buffer_size >= buffer_max_grow_size)
      target_buffer_size = buffer_max_grow_size;
      
    // Extra safety check
    if (target_buffer_size > 255) {
        ESP_LOGE(TAG, "CRITICAL: target_buffer_size overflow detected (%llu), resetting to safe value",
                 target_buffer_size);
        target_buffer_size = buffer_max_grow_size;
    }
    
    ESP_LOGI(TAG, "Buffer Underflow, New Size: %llu", (unsigned long long)target_buffer_size);
  }
  is_underrun = true;
}

// Enhanced push function with timestamp support
bool push_chunk_with_timestamp(uint8_t *chunk, uint64_t playout_time_ms, uint32_t rtp_timestamp) {
    // CRITICAL: Validate inputs first
    if (!packet_buffer || !chunk) {
        ESP_LOGE(TAG, "CRITICAL: NULL pointer - buffer not initialized or invalid chunk");
        return false;
    }
    
    uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
    
    // Validate max_buffer_size is reasonable
    if (max_buffer_size == 0) {
        ESP_LOGE(TAG, "CRITICAL: Invalid max_buffer_size: %d", max_buffer_size);
        return false;
    }
    
    // Sanity check - buffer size should never exceed max
    if (packet_buffer_size > max_buffer_size) {
        ESP_LOGE(TAG, "CRITICAL: packet_buffer_size (%llu) exceeds max (%d), resetting",
                 packet_buffer_size, max_buffer_size);
        packet_buffer_size = 0;
        packet_buffer_pos = 0;
    }
    
    // Get system time BEFORE entering critical section if we need it
    // This is needed for has_enough_future_packets() check
    uint64_t current_time_ms = 0;
    if (playout_time_ms > 0 && packet_metadata) {
        // Use monotonic system tick to match playout_time_ms domain
        current_time_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    }
    
    // Variables for logging outside critical section
    bool should_log_underflow = false;
    bool should_log_range = false;
    static uint32_t underflow_count = 0;
    static uint32_t range_error_count = 0;
    uint64_t log_playout_time = playout_time_ms;
    uint64_t log_current_time = current_time_ms;
    int64_t log_diff = 0;
    
    taskENTER_CRITICAL(&buffer_mutex);
    if (packet_buffer_size >= max_buffer_size) {
        // Use >= to be safe
        packet_buffer_size = target_buffer_size;
        if (packet_buffer_size > max_buffer_size) {
            packet_buffer_size = max_buffer_size - 1;
        }
        taskEXIT_CRITICAL(&buffer_mutex);
        ESP_LOGI(TAG, "Buffer Overflow");
        return false;
    }
    
    int write_position = (packet_buffer_pos + packet_buffer_size) % max_buffer_size;
    
    // CRITICAL: Validate write position before memcpy
    if (write_position < 0 || write_position >= max_buffer_size) {
        taskEXIT_CRITICAL(&buffer_mutex);
        ESP_LOGE(TAG, "CRITICAL: Invalid write position %d (max %d)", write_position, max_buffer_size);
        return false;
    }
    
    // CRITICAL: Validate buffer pointer before write
    if (!packet_buffer[write_position]) {
        taskEXIT_CRITICAL(&buffer_mutex);
        ESP_LOGE(TAG, "CRITICAL: NULL buffer at position %d", write_position);
        return false;
    }
    
    // Copy audio data - NOW SAFE
    memcpy(packet_buffer[write_position], chunk, PCM_CHUNK_SIZE);
    
    // Store metadata if metadata buffer is available
    if (packet_metadata) {
        // Enhanced validation - check for reasonable timestamp range
        bool timestamp_valid = true;
        
        if (playout_time_ms != 0) {  // 0 means no sync, which is valid
            // Check for obvious underflow (timestamps near UINT64_MAX)
            if (playout_time_ms > 0xFFFFFFFF00000000ULL) {
                underflow_count++;
                if (underflow_count % 100 == 0 || underflow_count == 1) {
                    should_log_underflow = true;
                }
                timestamp_valid = false;
            } else {
                // Check timestamp is within reasonable bounds (-1 sec to +10 sec from now)
                log_diff = (int64_t)playout_time_ms - (int64_t)current_time_ms;
                if (log_diff < -1000 || log_diff > 10000) {
                    range_error_count++;
                    if (range_error_count % 100 == 0 || range_error_count == 1) {
                        should_log_range = true;
                    }
                    timestamp_valid = false;
                }
            }
            
            if (!timestamp_valid) {
                playout_time_ms = 0;  // Fall back to immediate playback
            }
        }
        
        packet_metadata[write_position].playout_time_ms = playout_time_ms;
        packet_metadata[write_position].rtp_timestamp = rtp_timestamp;
        packet_metadata[write_position].flags = (playout_time_ms > 0 && timestamp_valid) ? 1 : 0;  // has_sync flag
        packet_metadata[write_position].reserved = 0;
    }
    
    packet_buffer_size++;
    received_packets++;
    
    // Sync-aware buffering logic
    bool log_underrun_change = false;
    bool log_legacy_exit = false;
    static bool last_underrun_state = true;
    bool has_enough = false;
    uint64_t log_buffer_size = packet_buffer_size;
    uint64_t log_received = received_packets;
    uint64_t log_target = target_buffer_size;
    
    if (playout_time_ms > 0 && packet_metadata) {
        // With sync: Check if we have enough future packets
        // Use the pre-calculated current time to avoid calling gettimeofday inside critical section
        has_enough = has_enough_future_packets_with_time(current_time_ms);
        is_underrun = !has_enough;
        
        // Check if underrun state changed
        if (is_underrun != last_underrun_state) {
            log_underrun_change = true;
        }
    } else {
        // Legacy: Use packet count
        if (received_packets >= target_buffer_size) {
            if (is_underrun) {
                log_legacy_exit = true;
            }
            is_underrun = false;
        }
    }
    
    taskEXIT_CRITICAL(&buffer_mutex);
    
    // Log validation errors outside critical section
    if (should_log_underflow) {
        ESP_LOGW(TAG, "Timestamp underflow detected: %llu (0x%llX), count=%lu, rtp_ts=%lu",
                log_playout_time, log_playout_time, underflow_count, rtp_timestamp);
    }
    if (should_log_range) {
        ESP_LOGW(TAG, "Timestamp out of range: playout=%llu, current=%llu, diff=%lld ms (count=%lu)",
                log_playout_time, log_current_time, log_diff, range_error_count);
    }
    
    // Log state changes outside critical section
    if (log_underrun_change) {
        ESP_LOGI(TAG, "Underrun state changed: %s -> %s (has_enough=%d, buffer_size=%llu)",
                 last_underrun_state ? "UNDERRUN" : "NORMAL",
                 is_underrun ? "UNDERRUN" : "NORMAL",
                 has_enough, log_buffer_size);
        last_underrun_state = is_underrun;
    }
    
    if (log_legacy_exit) {
        ESP_LOGI(TAG, "Exiting underrun (legacy mode): received=%llu, target=%llu",
                 log_received, log_target);
    }
    
    return true;
}

// Legacy wrapper for backward compatibility
bool push_chunk(uint8_t *chunk) {
    return push_chunk_with_timestamp(chunk, 0, 0);
}

// Enhanced pop function that considers timestamps
pop_result_t pop_chunk_timed(void) {
    pop_result_t result = {NULL, 0, false};
    
    static uint32_t call_count = 0;
    static uint32_t empty_buffer_count = 0;
    static uint32_t underrun_count = 0;
    static uint32_t too_early_count = 0;
    static uint32_t too_late_count = 0;
    static uint32_t success_count = 0;
    static TickType_t last_log = 0;
    
    call_count++;
    
    // Simple periodic logging
    TickType_t now = xTaskGetTickCount();
    if ((now - last_log) >= pdMS_TO_TICKS(5000)) {
        ESP_LOGI(TAG, "pop_chunk_timed stats: calls=%lu, empty=%lu, underrun=%lu, early=%lu, late=%lu, success=%lu",
                 call_count, empty_buffer_count, underrun_count, too_early_count, too_late_count, success_count);
        last_log = now;
        // Reset counters for next period
        call_count = 0;
        empty_buffer_count = 0;
        underrun_count = 0;
        too_early_count = 0;
        too_late_count = 0;
        success_count = 0;
    }
    
    // Get current time BEFORE entering critical section - use monotonic system tick
    uint64_t current_time_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    
    taskENTER_CRITICAL(&buffer_mutex);
    
    if (packet_buffer_size == 0) {
        empty_buffer_count++;
        taskEXIT_CRITICAL(&buffer_mutex);
        set_underrun();
        return result;
    }
    
    if (is_underrun) {
        underrun_count++;
        taskEXIT_CRITICAL(&buffer_mutex);
        return result;
    }
    
    uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
    
    // Safety check - ensure buffer positions are valid
    if (packet_buffer_pos >= max_buffer_size) {
        // Cannot log inside critical section - will cause deadlock
        packet_buffer_pos = 0;
        packet_buffer_size = 0;
        taskEXIT_CRITICAL(&buffer_mutex);
        ESP_LOGE(TAG, "CRITICAL: packet_buffer_pos >= max_buffer_size, reset buffer");
        set_underrun();
        return result;
    }
    
    // Variables for logging outside critical section (pop)
    bool should_log_corruption = false;
    static uint32_t corruption_count = 0;
    uint64_t log_playout = 0;
    uint32_t log_rtp_ts = 0;
    int64_t log_future_ms = 0;
    
    // Check if metadata is available and if this chunk has timing info
    if (packet_metadata) {
        chunk_metadata_t *metadata = &packet_metadata[packet_buffer_pos];
        
        // Check if it's time to play this chunk
        if (metadata->playout_time_ms > 0) {
            // Synchronized mode - use system time (already obtained before critical section)
            uint64_t now_ms = current_time_ms;
            
            // Enhanced validation: check for underflow and reasonable time bounds
            bool timestamp_corrupted = false;
            log_playout = metadata->playout_time_ms;
            log_rtp_ts = metadata->rtp_timestamp;
            
            // Check for obvious underflow (near UINT64_MAX)
            if (metadata->playout_time_ms > 0xFFFFFFFF00000000ULL) {
                timestamp_corrupted = true;
                corruption_count++;
                if (corruption_count % 100 == 0 || corruption_count == 1) {
                    should_log_corruption = true;
                }
            }
            // Check for unreasonable future times (>1 second ahead)
            else if (metadata->playout_time_ms > now_ms + 1000) {
                log_future_ms = (int64_t)metadata->playout_time_ms - (int64_t)now_ms;
                timestamp_corrupted = true;
                corruption_count++;
                if (corruption_count % 100 == 0 || corruption_count == 1) {
                    should_log_corruption = true;
                }
            }
            
            if (timestamp_corrupted) {
                // Reset to immediate playback
                metadata->playout_time_ms = 0;
                metadata->flags = 0;  // Clear has_sync flag
                // Continue with normal processing without timing check
            } else {
                int64_t time_diff = (int64_t)metadata->playout_time_ms - (int64_t)now_ms;
                
                if (time_diff > 10) {
                    // Too early, don't play yet
                    too_early_count++;
                    taskEXIT_CRITICAL(&buffer_mutex);
                    return result;
                } else if (time_diff < -(CONFIG_MAX_PLAYOUT_DELAY_MS / 2)) {
                    // Too late - behavior depends on sync mode
                    too_late_count++;
                    #ifdef CONFIG_SYNC_MODE_STRICT
                        // Strict mode: drop the packet
                        int64_t log_time_diff = time_diff;
                        packet_buffer_size--;
                        packet_buffer_pos = (packet_buffer_pos + 1) % max_buffer_size;
                        taskEXIT_CRITICAL(&buffer_mutex);
                        // Log after exiting critical section
                        ESP_LOGW(TAG, "Packet late by %lld ms, dropping (strict mode, count=%lu)", -log_time_diff, too_late_count);
                        return result;
                    #else
                        // Adaptive mode: continue processing without logging inside critical section
                        // Will log outside if needed
                    #endif
                }
            }
        }
        
        // Only set playout_time_ms if it's valid (not corrupted/reset)
        if (metadata->playout_time_ms > 0 && metadata->playout_time_ms < 0xFFFFFFFF00000000ULL) {
            result.playout_time_ms = metadata->playout_time_ms;
        } else {
            result.playout_time_ms = 0;  // No sync
        }
    }
    
    // CRITICAL: Validate position before accessing array
    if (packet_buffer_pos >= max_buffer_size) {
        packet_buffer_pos = 0;  // Reset to safe value
        taskEXIT_CRITICAL(&buffer_mutex);
        ESP_LOGE(TAG, "CRITICAL: Invalid buffer position in pop");
        set_underrun();
        return result;
    }
    
    // CRITICAL: Check for NULL pointer
    if (!packet_buffer || !packet_buffer[packet_buffer_pos]) {
        taskEXIT_CRITICAL(&buffer_mutex);
        ESP_LOGE(TAG, "CRITICAL: NULL buffer pointer at position %llu", packet_buffer_pos);
        return result;
    }
    
    // Pop the chunk - NOW SAFE
    result.data = packet_buffer[packet_buffer_pos];
    result.should_play_now = true;
    success_count++;
    
    packet_buffer_size--;
    packet_buffer_pos = (packet_buffer_pos + 1) % max_buffer_size;
    
    taskEXIT_CRITICAL(&buffer_mutex);
    
    // Log corruption outside critical section
    if (should_log_corruption) {
        if (log_playout > 0xFFFFFFFF00000000ULL) {
            ESP_LOGW(TAG, "Timestamp underflow in pop: %llu (0x%llX), rtp_ts=%lu, count=%lu",
                    log_playout, log_playout, log_rtp_ts, corruption_count);
        } else if (log_future_ms > 0) {
            ESP_LOGW(TAG, "Timestamp too far future: %lld ms ahead, playout=%llu, now=%llu (count=%lu)",
                    log_future_ms, log_playout, current_time_ms, corruption_count);
        }
    }
    
    return result;
}

// Legacy wrapper for backward compatibility
uint8_t *pop_chunk() {
    pop_result_t result = pop_chunk_timed();
    return result.data;
}

void empty_buffer() {
    taskENTER_CRITICAL(&buffer_mutex);
    packet_buffer_size = 0;
    received_packets = 0;
    
    // Clear metadata if available
    if (packet_metadata) {
        uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
        memset(packet_metadata, 0, sizeof(chunk_metadata_t) * max_buffer_size);
    }
    
    taskEXIT_CRITICAL(&buffer_mutex);
}

// Internal version that accepts pre-calculated time (for use inside critical sections)
static bool has_enough_future_packets_with_time(uint64_t now_ms) {
    if (packet_buffer_size == 0 || !packet_metadata) return false;
    
    uint64_t future_coverage_ms = 0;
    uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
    
    // Note: This function is called from inside critical section in push_chunk_with_timestamp
    // So we don't need additional locking here - the caller already has the lock
    
    // Limit iteration to actual buffer size to prevent overflow
    int iterations = (packet_buffer_size < max_buffer_size) ? packet_buffer_size : max_buffer_size;
    for (int i = 0; i < iterations; i++) {
        int pos = (packet_buffer_pos + i) % max_buffer_size;
        chunk_metadata_t *meta = &packet_metadata[pos];
        
        if (meta->playout_time_ms > now_ms) {
            future_coverage_ms = meta->playout_time_ms - now_ms;
        }
    }
    
    // Need at least CONFIG_MIN_FUTURE_BUFFER_MS of future audio
    return future_coverage_ms >= CONFIG_MIN_FUTURE_BUFFER_MS;
}

// Public version that gets the current time (for external callers)
bool has_enough_future_packets(void) {
    if (packet_buffer_size == 0 || !packet_metadata) return false;
    
    // Get system time - safe to call when not in critical section
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    
    return has_enough_future_packets_with_time(now_ms);
}

// Get buffer latency statistics
void get_buffer_stats(buffer_stats_t *stats) {
    if (!stats || !packet_metadata) return;
    
    taskENTER_CRITICAL(&buffer_mutex);
    
    stats->min_latency_ms = INT64_MAX;
    stats->max_latency_ms = INT64_MIN;
    stats->avg_latency_ms = 0;
    stats->synced_packets = 0;
    stats->unsynced_packets = 0;
    
    if (packet_buffer_size > 0) {
        uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
        int64_t total_latency = 0;
        
        // Store buffer info to get time after critical section
        int iterations = (packet_buffer_size < max_buffer_size) ? packet_buffer_size : max_buffer_size;
        
        // Exit critical section to get time
        taskEXIT_CRITICAL(&buffer_mutex);
        
        // NOW safe to get monotonic system time
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        
        // Re-enter critical section
        taskENTER_CRITICAL(&buffer_mutex);
        
        // Re-check buffer size in case it changed
        if (packet_buffer_size == 0) {
            taskEXIT_CRITICAL(&buffer_mutex);
            return;
        }
        
        // Re-calculate iterations in case buffer changed
        iterations = (packet_buffer_size < max_buffer_size) ? packet_buffer_size : max_buffer_size;
        for (int i = 0; i < iterations; i++) {
            int pos = (packet_buffer_pos + i) % max_buffer_size;
            chunk_metadata_t *meta = &packet_metadata[pos];
            
            if (meta->flags & 1) {  // has_sync flag
                stats->synced_packets++;
                int64_t latency = (int64_t)meta->playout_time_ms - (int64_t)now_ms;
                
                if (latency < stats->min_latency_ms) {
                    stats->min_latency_ms = latency;
                }
                if (latency > stats->max_latency_ms) {
                    stats->max_latency_ms = latency;
                }
                total_latency += latency;
            } else {
                stats->unsynced_packets++;
            }
        }
        
        if (stats->synced_packets > 0) {
            stats->avg_latency_ms = total_latency / stats->synced_packets;
        }
    }
    
    taskEXIT_CRITICAL(&buffer_mutex);
}

void setup_buffer() {
    ESP_LOGI(TAG, "Allocating buffer with timestamp support");
    if (packet_buffer) {
      ESP_LOGI(TAG, "Already allocated");
      return;
    }
    uint8_t initial_buffer_size = lifecycle_get_initial_buffer_size();
    uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
    target_buffer_size = initial_buffer_size;
    
    // Initialize cached buffer growth parameters
    buffer_grow_step_size = lifecycle_get_buffer_grow_step_size();
    buffer_max_grow_size = lifecycle_get_max_grow_size();
    
    ESP_LOGI(TAG, "Buffer sizes: initial=%d, max=%d, chunk=%d bytes",
             initial_buffer_size, max_buffer_size, PCM_CHUNK_SIZE);
    ESP_LOGI(TAG, "Buffer growth: step=%d, max_grow=%d",
             buffer_grow_step_size, buffer_max_grow_size);
    
    // Allocate the array of packet pointers
    packet_buffer = (uint8_t **)malloc(sizeof(uint8_t *) * max_buffer_size);
    if (!packet_buffer) {
        ESP_LOGE(TAG, "Failed to allocate packet buffer array");
        return;
    }
    
    // Allocate the actual buffer memory
    size_t total_size = (size_t)PCM_CHUNK_SIZE * (size_t)max_buffer_size;
    
    // Check for overflow in size calculation
    if (total_size / PCM_CHUNK_SIZE != max_buffer_size) {
        ESP_LOGE(TAG, "CRITICAL: Buffer size calculation overflow");
        heap_caps_free(packet_buffer);
        packet_buffer = NULL;
        return;
    }
    
    uint8_t *buffer = (uint8_t *)malloc(total_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer memory of size %zu", total_size);
        heap_caps_free(packet_buffer);
        packet_buffer = NULL;
        return;
    }
    
    memset(buffer, 0, total_size);
    for (int i = 0; i < max_buffer_size; i++) {
        packet_buffer[i] = buffer + (i * PCM_CHUNK_SIZE);
    }
    
    // Allocate parallel metadata buffer
    packet_metadata = (chunk_metadata_t *)malloc(sizeof(chunk_metadata_t) * max_buffer_size);
    if (!packet_metadata) {
        ESP_LOGE(TAG, "Failed to allocate metadata buffer");
        // Continue without metadata support - backward compatibility
    } else {
        memset(packet_metadata, 0, sizeof(chunk_metadata_t) * max_buffer_size);
        ESP_LOGI(TAG, "Metadata buffer allocated: %d entries, %d bytes per entry",
                 max_buffer_size, sizeof(chunk_metadata_t));
    }
    
    ESP_LOGI(TAG, "Buffer allocated with initial size %d, max size %d, metadata support: %s",
             initial_buffer_size, max_buffer_size, packet_metadata ? "enabled" : "disabled");
}

esp_err_t buffer_update_growth_params() {
  uint8_t new_grow_step_size = lifecycle_get_buffer_grow_step_size();
  uint8_t new_max_grow_size = lifecycle_get_max_grow_size();
  
  // Update parameters in thread-safe manner
  taskENTER_CRITICAL(&buffer_mutex);
  buffer_grow_step_size = new_grow_step_size;
  buffer_max_grow_size = new_max_grow_size;
  
  // Ensure current target_buffer_size doesn't exceed new max grow size
  if (target_buffer_size > buffer_max_grow_size) {
    target_buffer_size = buffer_max_grow_size;
  }
  taskEXIT_CRITICAL(&buffer_mutex);
  
  ESP_LOGI(TAG, "Updated buffer growth params: step=%d, max_grow=%d",
           new_grow_step_size, new_max_grow_size);
  
  return ESP_OK;
}
