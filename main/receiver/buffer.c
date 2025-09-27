#include "esp_psram.h"
#include "global.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "lifecycle_manager.h"

// Flag if the stream is currently underrun and rebuffering
bool is_underrun                            = true;
// Number of received packets since last underflow
unsigned int received_packets               = 0;
// Number of packets in ring buffer
unsigned int packet_buffer_size             = 0;
// Position of ring buffer read head
unsigned int packet_buffer_pos              = 0;
// Number of bytes to buffer
unsigned int target_buffer_size             = 0;
// Buffer of packets to send
uint8_t **packet_buffer = NULL;
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
    ESP_LOGI(TAG, "Buffer Underflow, New Size: %i", target_buffer_size);
  }
  is_underrun = true;
}

bool push_chunk(uint8_t *chunk) {
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
  memcpy(packet_buffer[write_position], chunk, PCM_CHUNK_SIZE);
  packet_buffer_size++;
  received_packets++;
  if (received_packets >= target_buffer_size)
    is_underrun = false;
  taskEXIT_CRITICAL(&buffer_mutex);
  return true;
}

uint8_t *pop_chunk() {
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
  uint8_t *return_chunk = packet_buffer[packet_buffer_pos];
  packet_buffer_size--;
  packet_buffer_pos = (packet_buffer_pos + 1) % max_buffer_size;
  taskEXIT_CRITICAL(&buffer_mutex);
  return return_chunk;
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
  
  // Allocate the array of packet pointers
  packet_buffer = (uint8_t **)malloc(sizeof(uint8_t *) * max_buffer_size);
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
    packet_buffer[i] = buffer + i * PCM_CHUNK_SIZE;
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
