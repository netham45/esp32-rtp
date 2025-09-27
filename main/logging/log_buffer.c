#include "log_buffer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_system.h"

// Ensure you have this defined in the header file (e.g., log_buffer.h)
// #define LOG_LINE_MAX_LENGTH 256
// #define LOG_BUFFER_SIZE_DEFAULT 2048

static const char *TAG = "log_buffer";

// Global log buffer instance
static log_buffer_t log_buffer = {
    .buffer = NULL,
    .size = 0,
    .head = 0,
    .tail = 0,
    .bytes_written = 0,
    .mutex = NULL,
    .initialized = false,
    .serial_output_enabled = true,
    .timestamps_enabled = false,
    .original_vprintf = NULL,
    .min_level = ESP_LOG_INFO  // Default to INFO level
};

// Static buffer allocation to avoid fragmentation
static char static_log_buffer[LOG_BUFFER_SIZE_DEFAULT];

// Forward declaration of custom vprintf handler
static int log_buffer_vprintf(const char *fmt, va_list args);

esp_err_t log_buffer_init(void) {
    log_buffer_config_t config = {
        .buffer_size = LOG_BUFFER_SIZE_DEFAULT,
        .enable_serial_output = true,
        .add_timestamps = false,
        .min_level = ESP_LOG_INFO
    };
    return log_buffer_init_with_config(&config);
}

esp_err_t log_buffer_init_with_config(const log_buffer_config_t *config) {
    if (log_buffer.initialized) {
        ESP_LOGW(TAG, "Log buffer already initialized");
        return ESP_OK;
    }

    if (!config || config->buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    log_buffer.mutex = xSemaphoreCreateMutex();
    if (log_buffer.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    if (config->buffer_size <= LOG_BUFFER_SIZE_DEFAULT) {
        log_buffer.buffer = static_log_buffer;
        log_buffer.size = config->buffer_size;
    } else {
        log_buffer.buffer = (char *)malloc(config->buffer_size);
        if (log_buffer.buffer == NULL) {
            vSemaphoreDelete(log_buffer.mutex);
            ESP_LOGE(TAG, "Failed to allocate buffer");
            return ESP_ERR_NO_MEM;
        }
        log_buffer.size = config->buffer_size;
    }

    log_buffer.head = 0;
    log_buffer.tail = 0;
    log_buffer.bytes_written = 0;
    log_buffer.serial_output_enabled = config->enable_serial_output;
    log_buffer.timestamps_enabled = config->add_timestamps;
    log_buffer.min_level = config->min_level;
    log_buffer.initialized = true;

    memset(log_buffer.buffer, 0, log_buffer.size);

    log_buffer.original_vprintf = esp_log_set_vprintf(log_buffer_vprintf);

    ESP_LOGI(TAG, "Log buffer initialized with %zu bytes", log_buffer.size);
    return ESP_OK;
}

// Helper function to check if a log message should be captured based on level
// This version checks the format string, which is more efficient.
static bool should_capture_log(const char *fmt) {
    if (!fmt) return true; // Capture if format is null

    // Check for ESP-IDF log level prefixes in the format string
    // Format is typically: "X (%d) %s: " where X is E/W/I/D/V
    if (fmt[0] == 'E' && fmt[1] == ' ') {
        return ESP_LOG_ERROR >= log_buffer.min_level;
    } else if (fmt[0] == 'W' && fmt[1] == ' ') {
        return ESP_LOG_WARN >= log_buffer.min_level;
    } else if (fmt[0] == 'I' && fmt[1] == ' ') {
        return ESP_LOG_INFO >= log_buffer.min_level;
    } else if (fmt[0] == 'D' && fmt[1] == ' ') {
        return ESP_LOG_DEBUG >= log_buffer.min_level;
    } else if (fmt[0] == 'V' && fmt[1] == ' ') {
        return ESP_LOG_VERBOSE >= log_buffer.min_level;
    }

    // If no recognized level prefix, capture it by default
    return true;
}

static int log_buffer_vprintf(const char *fmt, va_list args) {
    if (!log_buffer.initialized) {
        return 0;
    }

    int written = 0;
    
    // First, check if we should capture this log based on its format string
    bool should_capture = should_capture_log(fmt);

    // Pass through to serial output if enabled. This uses the original vprintf correctly.
    if (log_buffer.serial_output_enabled && log_buffer.original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        written = log_buffer.original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }
    
    // If we shouldn't capture it in our buffer, we're done.
    if (!should_capture) {
        return written;
    }
    
    // Use a static buffer to format the message for the ring buffer, avoiding the stack.
    static char log_line[LOG_LINE_MAX_LENGTH];
    int len_for_buffer = 0;
    
    // Lock the mutex to protect the static log_line and the ring buffer
    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) != pdTRUE) {
        return written; // Could not get mutex, bail out
    }
    
    // Add timestamp if enabled
    if (log_buffer.timestamps_enabled) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm timeinfo;
        localtime_r(&tv.tv_sec, &timeinfo);
        
        len_for_buffer = snprintf(log_line, sizeof(log_line),
                                  "[%02d:%02d:%02d.%03ld] ",
                                  timeinfo.tm_hour,
                                  timeinfo.tm_min,
                                  timeinfo.tm_sec,
                                  tv.tv_usec / 1000);
    }

    // Format the actual log message into our static buffer
    int msg_len = vsnprintf(log_line + len_for_buffer, sizeof(log_line) - len_for_buffer, fmt, args);
    if (msg_len > 0) {
        len_for_buffer += msg_len;
    }

    // Write the final formatted string to the ring buffer
    if (len_for_buffer > 0) {
        for (int i = 0; i < len_for_buffer; i++) {
            log_buffer.buffer[log_buffer.head] = log_line[i];
            log_buffer.head = (log_buffer.head + 1) % log_buffer.size;
            
            // Handle overflow by moving the tail forward
            if (log_buffer.head == log_buffer.tail) {
                log_buffer.tail = (log_buffer.tail + 1) % log_buffer.size;
            }
        }
        log_buffer.bytes_written += len_for_buffer;
    }

    xSemaphoreGive(log_buffer.mutex);

    // If serial was disabled, we return the length of the message that would have been written
    return written > 0 ? written : msg_len;
}


size_t log_buffer_write(const char *data, size_t len) {
    if (!log_buffer.initialized || !data || len == 0) {
        return 0;
    }

    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t written = 0;
    
    for (size_t i = 0; i < len; i++) {
        log_buffer.buffer[log_buffer.head] = data[i];
        log_buffer.head = (log_buffer.head + 1) % log_buffer.size;
        
        if (log_buffer.head == log_buffer.tail) {
            log_buffer.tail = (log_buffer.tail + 1) % log_buffer.size;
        }
        
        written++;
        log_buffer.bytes_written++;
    }

    xSemaphoreGive(log_buffer.mutex);
    return written;
}

size_t log_buffer_read(char *dest, size_t max_len) {
    if (!log_buffer.initialized || !dest || max_len == 0) {
        return 0;
    }

    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t read = 0;
    
    while (log_buffer.tail != log_buffer.head && read < max_len) {
        dest[read] = log_buffer.buffer[log_buffer.tail];
        log_buffer.tail = (log_buffer.tail + 1) % log_buffer.size;
        read++;
    }

    xSemaphoreGive(log_buffer.mutex);
    return read;
}

size_t log_buffer_peek(char *dest, size_t max_len) {
    if (!log_buffer.initialized || !dest || max_len == 0) {
        return 0;
    }

    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t read = 0;
    size_t temp_tail = log_buffer.tail;
    
    while (temp_tail != log_buffer.head && read < max_len) {
        dest[read] = log_buffer.buffer[temp_tail];
        temp_tail = (temp_tail + 1) % log_buffer.size;
        read++;
    }

    xSemaphoreGive(log_buffer.mutex);
    return read;
}

size_t log_buffer_peek_latest(char *dest, size_t max_len) {
    if (!log_buffer.initialized || !dest || max_len == 0) {
        return 0;
    }

    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t available = 0;
    if (log_buffer.head >= log_buffer.tail) {
        available = log_buffer.head - log_buffer.tail;
    } else {
        available = log_buffer.size - log_buffer.tail + log_buffer.head;
    }

    // If we have more data than requested, start from a later position
    size_t start_offset = 0;
    if (available > max_len) {
        start_offset = available - max_len;
    }

    // Calculate the starting position
    size_t temp_tail = (log_buffer.tail + start_offset) % log_buffer.size;

    size_t read = 0;
    while (temp_tail != log_buffer.head && read < max_len) {
        dest[read] = log_buffer.buffer[temp_tail];
        temp_tail = (temp_tail + 1) % log_buffer.size;
        read++;
    }

    xSemaphoreGive(log_buffer.mutex);
    return read;
}

void log_buffer_clear(void) {
    if (!log_buffer.initialized) {
        return;
    }

    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) == pdTRUE) {
        log_buffer.head = 0;
        log_buffer.tail = 0;
        log_buffer.bytes_written = 0;
        memset(log_buffer.buffer, 0, log_buffer.size);
        xSemaphoreGive(log_buffer.mutex);
    }
}

size_t log_buffer_get_used(void) {
    if (!log_buffer.initialized) {
        return 0;
    }

    size_t used = 0;
    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) == pdTRUE) {
        if (log_buffer.head >= log_buffer.tail) {
            used = log_buffer.head - log_buffer.tail;
        } else {
            used = log_buffer.size - log_buffer.tail + log_buffer.head;
        }
        xSemaphoreGive(log_buffer.mutex);
    }
    return used;
}

size_t log_buffer_get_size(void) {
    return log_buffer.size;
}

bool log_buffer_has_overflowed(void) {
    if (!log_buffer.initialized) {
        return false;
    }
    // A more robust check for overflow
    return log_buffer.bytes_written >= log_buffer.size;
}

void log_buffer_set_serial_output(bool enabled) {
    if (log_buffer.initialized) {
        log_buffer.serial_output_enabled = enabled;
    }
}

void log_buffer_set_timestamps(bool enabled) {
    if (log_buffer.initialized) {
        log_buffer.timestamps_enabled = enabled;
    }
}

esp_err_t log_buffer_set_min_level(esp_log_level_t level) {
    if (!log_buffer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (level > ESP_LOG_VERBOSE) { // ESP_LOG_NONE is 0
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) == pdTRUE) {
        log_buffer.min_level = level;
        xSemaphoreGive(log_buffer.mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_log_level_t log_buffer_get_min_level(void) {
    if (!log_buffer.initialized) {
        return ESP_LOG_INFO;
    }
    
    esp_log_level_t level = ESP_LOG_INFO;
    if (xSemaphoreTake(log_buffer.mutex, portMAX_DELAY) == pdTRUE) {
        level = log_buffer.min_level;
        xSemaphoreGive(log_buffer.mutex);
    }
    
    return level;
}

void log_buffer_deinit(void) {
    if (!log_buffer.initialized) {
        return;
    }

    if (log_buffer.original_vprintf) {
        esp_log_set_vprintf(log_buffer.original_vprintf);
    }

    if (log_buffer.mutex) {
        vSemaphoreDelete(log_buffer.mutex);
    }

    if (log_buffer.buffer != static_log_buffer && log_buffer.buffer != NULL) {
        free(log_buffer.buffer);
    }

    // Reset all fields
    memset(&log_buffer, 0, sizeof(log_buffer));
}