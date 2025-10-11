#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define LOG_BUFFER_SIZE_DEFAULT (1024 * 4) // 4KB
#define LOG_LINE_MAX_LENGTH 256

typedef struct {
    size_t buffer_size;           // Total buffer size
    bool enable_serial_output;    // Continue outputting to serial console
    bool add_timestamps;          // Add timestamps to log entries
    esp_log_level_t min_level;    // Minimum log level to capture
} log_buffer_config_t;

typedef struct {
    char *buffer;                 // Pointer to the ring buffer
    size_t size;                  // Total size of the buffer
    size_t head;                  // Write position
    size_t tail;                  // Read position
    size_t bytes_written;         // Total bytes written (for overflow detection)
    SemaphoreHandle_t mutex;      // Mutex for thread-safe access
    bool initialized;             // Initialization flag
    bool serial_output_enabled;   // Whether to continue serial output
    bool timestamps_enabled;      // Whether to add timestamps
    vprintf_like_t original_vprintf; // Original vprintf handler
    esp_log_level_t min_level;    // Minimum log level to capture
} log_buffer_t;

// Initialize the log buffer system with default configuration
esp_err_t log_buffer_init(void);

// Initialize the log buffer system with custom configuration
esp_err_t log_buffer_init_with_config(const log_buffer_config_t *config);

// Write data to the log buffer (internal use by vprintf handler)
size_t log_buffer_write(const char *data, size_t len);

// Read data from the log buffer
size_t log_buffer_read(char *dest, size_t max_len);

// Peek at data in the log buffer without removing it
size_t log_buffer_peek(char *dest, size_t max_len);

// Peek at the latest data in the log buffer without removing it
// Returns the newest logs (from the end of the buffer)
size_t log_buffer_peek_latest(char *dest, size_t max_len);

// Clear the log buffer
void log_buffer_clear(void);

// Get the number of bytes currently in the buffer
size_t log_buffer_get_used(void);

// Get the total size of the buffer
size_t log_buffer_get_size(void);

// Check if buffer has overflowed (written more than size)
bool log_buffer_has_overflowed(void);

// Enable/disable serial output passthrough
void log_buffer_set_serial_output(bool enabled);

// Enable/disable timestamps
void log_buffer_set_timestamps(bool enabled);

// Set the minimum log level to capture in the buffer
esp_err_t log_buffer_set_min_level(esp_log_level_t level);

// Get the current minimum log level
esp_log_level_t log_buffer_get_min_level(void);

// Deinitialize the log buffer system
void log_buffer_deinit(void);

#endif // LOG_BUFFER_H