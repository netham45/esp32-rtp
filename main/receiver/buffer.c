#include "buffer.h"
#include "esp_psram.h"
#include "global.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "lifecycle_manager.h"

// Flag if the stream is currently underrun and rebuffering
bool is_underrun                            = true;
// Number of received packets since last underflow
uint64_t received_packets                   = 0;
// Number of packets in ring buffer
uint64_t packet_buffer_size                 = 0;
// Position of ring buffer read head
uint64_t packet_buffer_pos                  = 0;
// Number of bytes to buffer
uint64_t target_buffer_size                 = 0;
// Buffer of packets to send
packet_with_ts_t *packet_buffer = NULL;

portMUX_TYPE buffer_mutex = portMUX_INITIALIZER_UNLOCKED;

// Cached buffer growth parameters for thread-safe access
static uint8_t buffer_grow_step_size = 0;
static uint8_t buffer_max_grow_size = 0;

void set_underrun() {
  if (!is_underrun) {
    received_packets = 0;
    target_buffer_size += buffer_grow_step_size;
    if (target_buffer_size >= buffer_max_grow_size)
      target_buffer_size = buffer_max_grow_size;
    ESP_LOGI(TAG, "Buffer Underflow, New Size: %llu", (unsigned long long)target_buffer_size);
  }
  is_underrun = true;
}

bool push_chunk_with_timestamp(uint8_t *chunk, uint64_t timestamp) {
  uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
  int write_position = (packet_buffer_pos + packet_buffer_size) % max_buffer_size;
//  ESP_LOGI(TAG, "memcpy(%p, %p, %i)", packet_buffer[write_position], chunk, PCM_CHUNK_SIZE);
  taskENTER_CRITICAL(&buffer_mutex);
  if (packet_buffer_size == max_buffer_size) {
    packet_buffer_size = target_buffer_size;
    taskEXIT_CRITICAL(&buffer_mutex);
    ESP_LOGI(TAG, "Buffer Overflow");
    return false;
  }

  write_position = (packet_buffer_pos + packet_buffer_size) % max_buffer_size;
  memcpy(packet_buffer[write_position].packet_buffer, chunk, PCM_CHUNK_SIZE);
  packet_buffer[write_position].timestamp = timestamp; // Use provided timestamp directly
  packet_buffer[write_position].skip_bytes = 0; // Default: no skip
  packet_buffer_size++;
  received_packets++;
  if (received_packets >= target_buffer_size)
    is_underrun = false;
  taskEXIT_CRITICAL(&buffer_mutex);
  return true;
}

bool push_chunk_with_skip(uint8_t *chunk, uint64_t timestamp, uint16_t skip_bytes) {
  uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
  int write_position = (packet_buffer_pos + packet_buffer_size) % max_buffer_size;
  
  taskENTER_CRITICAL(&buffer_mutex);
  if (packet_buffer_size == max_buffer_size) {
    packet_buffer_size = target_buffer_size;
    taskEXIT_CRITICAL(&buffer_mutex);
    ESP_LOGI(TAG, "Buffer Overflow");
    return false;
  }

  write_position = (packet_buffer_pos + packet_buffer_size) % max_buffer_size;
  memcpy(packet_buffer[write_position].packet_buffer, chunk, PCM_CHUNK_SIZE);
  packet_buffer[write_position].timestamp = timestamp;
  packet_buffer[write_position].skip_bytes = skip_bytes;  // Set skip bytes
  packet_buffer_size++;
  received_packets++;
  if (received_packets >= target_buffer_size)
    is_underrun = false;
  taskEXIT_CRITICAL(&buffer_mutex);
  return true;
}

bool push_chunk(uint8_t *chunk) {
  // Backward compatible wrapper - maintains current behavior with +5ms delay
  return push_chunk_with_timestamp(chunk, esp_timer_get_time() + 5000);
}

packet_with_ts_t *pop_chunk() {
  taskENTER_CRITICAL(&buffer_mutex);
  if (packet_buffer_size == 0) {
    taskEXIT_CRITICAL(&buffer_mutex);
    set_underrun();
    return NULL;
  }
  if (is_underrun) {
    taskEXIT_CRITICAL(&buffer_mutex);
    return NULL;
  }
  uint8_t max_buffer_size = lifecycle_get_max_buffer_size();
  while (packet_buffer[packet_buffer_pos].timestamp > esp_timer_get_time()) {
    taskEXIT_CRITICAL(&buffer_mutex);
    vTaskDelay(0);
    taskENTER_CRITICAL(&buffer_mutex);
  }
  
  // Get pointer to current packet
  packet_with_ts_t *packet = &packet_buffer[packet_buffer_pos];
  
  packet_buffer_size--;
  packet_buffer_pos = (packet_buffer_pos + 1) % max_buffer_size;
  taskEXIT_CRITICAL(&buffer_mutex);
  return packet;
}

void empty_buffer() {
	taskENTER_CRITICAL(&buffer_mutex);
	packet_buffer_size = 0;
	received_packets = 0;
	taskEXIT_CRITICAL(&buffer_mutex);
}

void setup_buffer() {
  ESP_LOGI(TAG, "Allocating buffer");
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
  
  // Allocate the array of packet structs
  packet_buffer = (packet_with_ts_t *)malloc(sizeof(packet_with_ts_t) * max_buffer_size);
  if (!packet_buffer) {
    ESP_LOGE(TAG, "Failed to allocate packet buffer array");
    return;
  }
  
  // Allocate the actual buffer memory
  uint8_t *buffer = (uint8_t *)malloc(PCM_CHUNK_SIZE * max_buffer_size);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffer memory");
    heap_caps_free(packet_buffer);
    packet_buffer = NULL;
    return;
  }
  
  memset(buffer, 0, PCM_CHUNK_SIZE * max_buffer_size);
  for (int i = 0; i < max_buffer_size; i++) {
    packet_buffer[i].packet_buffer = buffer + i * PCM_CHUNK_SIZE;
    packet_buffer[i].timestamp = 0;
    packet_buffer[i].skip_bytes = 0;
  }
  ESP_LOGI(TAG, "Buffer allocated with initial size %d, max size %d", initial_buffer_size, max_buffer_size);
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
