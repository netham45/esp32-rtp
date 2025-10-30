#include "pcm_visualizer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char* TAG = "pcm_viz";

// Auto-gain tracking
static float s_peak_history[100] = {0};  // Store last 100 peak measurements
static int s_peak_history_idx = 0;
static uint32_t s_last_gain_adjust_time = 0;

// Ring buffer for PCM data
static uint8_t ring_buffer[PCM_VIZ_RING_SIZE];
static volatile uint32_t write_ptr = 0;
static volatile uint32_t read_ptr = 0;

// Sample windows for RMS calculation
static float samples_left[PCM_VIZ_WINDOW_SIZE];
static float samples_right[PCM_VIZ_WINDOW_SIZE];
static uint32_t sample_index = 0;

// Loudness values
static pcm_viz_loudness_t current_loudness = {0};

// Configuration
static float viz_gain = 1.0f;  // Unit gain for PPM meter
static bool is_initialized = false;

// VU meter state with ballistics
static float s_left_vu_db = PCM_VIZ_MIN_DB;
static float s_right_vu_db = PCM_VIZ_MIN_DB;

// Auto-gain state
static float s_auto_gain = 1.0f;

// Task and synchronization
static TaskHandle_t viz_task_handle = NULL;
static SemaphoreHandle_t data_mutex = NULL;

// Statistics
static pcm_viz_stats_t viz_stats = {0};

// Legacy peak decay (will be replaced by proper peak hold)
#define PEAK_DECAY_RATE 0.95f

// Internal functions
static void pcm_viz_task(void* param);
static void process_ring_buffer(void);
static void calculate_rms_loudness(void);
static size_t get_buffer_bytes_available(void);

// Get number of bytes available in ring buffer
static size_t get_buffer_bytes_available(void) {
    uint32_t w = write_ptr;
    uint32_t r = read_ptr;

    if (w >= r) {
        return w - r;
    } else {
        return PCM_VIZ_RING_SIZE - r + w;
    }
}

esp_err_t pcm_viz_init(void) {
    if (is_initialized) {
        ESP_LOGW(TAG, "Visualizer already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing PCM visualizer (loudness mode)");

    // Create mutex for thread safety
    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Initialize buffers
    memset(ring_buffer, 0, sizeof(ring_buffer));
    memset(samples_left, 0, sizeof(samples_left));
    memset(samples_right, 0, sizeof(samples_right));
    write_ptr = 0;
    read_ptr = 0;
    sample_index = 0;

    // Initialize loudness values
    memset(&current_loudness, 0, sizeof(current_loudness));
    
    // Initialize PPM meter dB values to scale minimum
    current_loudness.left_db = PCM_VIZ_PPM_MIN_DB;      // -40 dB
    current_loudness.right_db = PCM_VIZ_PPM_MIN_DB;     // -40 dB
    current_loudness.left_peak_db = PCM_VIZ_PPM_MIN_DB;
    current_loudness.right_peak_db = PCM_VIZ_PPM_MIN_DB;
    
    // Initialize VU meter state (legacy, unused but keep for consistency)
    s_left_vu_db = PCM_VIZ_PPM_MIN_DB;
    s_right_vu_db = PCM_VIZ_PPM_MIN_DB;
    s_auto_gain = 1.0f;
    s_peak_history_idx = 0;
    s_last_gain_adjust_time = esp_timer_get_time() / 1000;
    memset(s_peak_history, 0, sizeof(s_peak_history));

    // Create processing task
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        pcm_viz_task,
        "pcm_viz",
        4096,
        NULL,
        5,  // Priority
        &viz_task_handle,
        0   // Core 0
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create visualizer task");
        vSemaphoreDelete(data_mutex);
        return ESP_FAIL;
    }

    is_initialized = true;
    ESP_LOGI(TAG, "PCM visualizer initialized (Memory: ~%.1fKB)",
             (sizeof(ring_buffer) + sizeof(samples_left) + sizeof(samples_right)) / 1024.0f);

    return ESP_OK;
}

esp_err_t pcm_viz_deinit(void) {
    if (!is_initialized) {
        return ESP_OK;
    }

    // Stop task
    if (viz_task_handle != NULL) {
        vTaskDelete(viz_task_handle);
        viz_task_handle = NULL;
    }

    // Delete mutex
    if (data_mutex != NULL) {
        vSemaphoreDelete(data_mutex);
        data_mutex = NULL;
    }

    is_initialized = false;
    ESP_LOGI(TAG, "PCM visualizer deinitialized");

    return ESP_OK;
}

esp_err_t pcm_viz_write(const uint8_t* data, size_t len) {
    if (!is_initialized || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // CRITICAL: Align write pointer to 4-byte boundary (stereo frame)
    // This ensures every chunk starts with LEFT channel
    if (write_ptr % 4 != 0) {
        write_ptr = ((write_ptr + 3) / 4) * 4;  // Round up to next frame boundary
        write_ptr %= PCM_VIZ_RING_SIZE;
    }

    // Check for buffer space
    size_t available = PCM_VIZ_RING_SIZE - get_buffer_bytes_available() - 1;
    if (len > available) {
        viz_stats.buffer_overruns++;
        // Allow overwrite of old data
    }

    // Write data to ring buffer
    for (size_t i = 0; i < len; i++) {
        ring_buffer[write_ptr] = data[i];
        write_ptr = (write_ptr + 1) % PCM_VIZ_RING_SIZE;

        // Handle overrun by moving read pointer to next frame boundary (4 bytes = 1 stereo frame)
        if (write_ptr == read_ptr) {
            // Advance by 4 bytes to maintain stereo frame alignment
            read_ptr = (read_ptr + 4) % PCM_VIZ_RING_SIZE;
        }
    }

    return ESP_OK;
}

static void process_ring_buffer(void) {
    // Process available samples
    while (get_buffer_bytes_available() >= 4) {  // Need at least 2 samples (L+R)
        // Read 16-bit stereo samples
        int16_t left, right;

        // Read left channel
        left = (int16_t)(ring_buffer[read_ptr] | (ring_buffer[(read_ptr + 1) % PCM_VIZ_RING_SIZE] << 8));
        read_ptr = (read_ptr + 2) % PCM_VIZ_RING_SIZE;

        // Read right channel
        right = (int16_t)(ring_buffer[read_ptr] | (ring_buffer[(read_ptr + 1) % PCM_VIZ_RING_SIZE] << 8));
        read_ptr = (read_ptr + 2) % PCM_VIZ_RING_SIZE;

        // Normalize to -1.0 to 1.0 (gain applied during RMS calculation)
        samples_left[sample_index] = left / 32768.0f;
        samples_right[sample_index] = right / 32768.0f;

        sample_index++;

        // Calculate RMS when window is full
        if (sample_index >= PCM_VIZ_WINDOW_SIZE) {
            calculate_rms_loudness();
            sample_index = 0;
        }

        viz_stats.samples_processed++;
    }
}

/**
 * Convert RMS value to dB
 */
static inline float rms_to_db(float rms) {
    return 20.0f * log10f(rms + 1e-10f);
}

/**
 * Map dB to normalized 0.0-1.0 range
 */
static inline float db_to_normalized(float db) {
    float normalized = (db - PCM_VIZ_MIN_DB) / PCM_VIZ_DB_RANGE;
    return fminf(fmaxf(normalized, 0.0f), 1.0f);
}

/**
 * Apply VU meter ballistics (fast attack, slow release)
 */
static float apply_ballistics(float current_db, float new_db) {
    if (new_db > current_db) {
        // Fast attack
        return current_db + (new_db - current_db) * PCM_VIZ_ATTACK_COEFF;
    } else {
        // Slow release
        return current_db + (new_db - current_db) * PCM_VIZ_RELEASE_COEFF;
    }
}

/**
 * Update auto-gain based on recent peaks
 */
static void update_auto_gain(float current_peak_normalized) {
    uint32_t now = esp_timer_get_time() / 1000; // Convert to milliseconds
    
    // Store current peak in history
    s_peak_history[s_peak_history_idx] = current_peak_normalized;
    s_peak_history_idx = (s_peak_history_idx + 1) % 100;
    
    // Only adjust gain once per second
    if ((now - s_last_gain_adjust_time) < 1000) {
        return;
    }
    s_last_gain_adjust_time = now;
    
    // Find maximum peak in recent history
    float max_peak = 0.0f;
    for (int i = 0; i < 100; i++) {
        if (s_peak_history[i] > max_peak) {
            max_peak = s_peak_history[i];
        }
    }
    
    // Adjust gain based on peak level
    if (max_peak > 0.95f) {
        // Peak too high, reduce gain
        s_auto_gain -= s_auto_gain * PCM_VIZ_AUTO_GAIN_ADJUST_RATE;
    } else if (max_peak < 0.6f && max_peak > 0.01f) {
        // Peak too low (but not silence), increase gain
        s_auto_gain += s_auto_gain * PCM_VIZ_AUTO_GAIN_ADJUST_RATE;
    }
    
    // Clamp gain to reasonable limits
    if (s_auto_gain < PCM_VIZ_AUTO_GAIN_MIN) s_auto_gain = PCM_VIZ_AUTO_GAIN_MIN;
    if (s_auto_gain > PCM_VIZ_AUTO_GAIN_MAX) s_auto_gain = PCM_VIZ_AUTO_GAIN_MAX;
}

/**
 * Update peak hold with decay
 */
static void update_peak_hold(float current_db, float *peak_hold_db, uint32_t *peak_hold_time) {
    uint32_t now = esp_timer_get_time() / 1000; // Convert to milliseconds
    
    if (current_db > *peak_hold_db) {
        // New peak detected
        *peak_hold_db = current_db;
        *peak_hold_time = now;
    } else {
        // Check if hold time expired
        uint32_t elapsed = now - *peak_hold_time;
        
        if (elapsed > PCM_VIZ_PEAK_HOLD_MS) {
            // Apply decay
            float decay_time_sec = (float)(elapsed - PCM_VIZ_PEAK_HOLD_MS) / 1000.0f;
            float decay_db = PCM_VIZ_PEAK_DECAY_DB_PER_SEC * decay_time_sec;
            *peak_hold_db = *peak_hold_db - decay_db;
            
            // Don't let it go below current level
            if (*peak_hold_db < current_db) {
                *peak_hold_db = current_db;
                *peak_hold_time = now;
            }
        }
    }
}

/**
 * Apply PPM-style ballistics (fast integration, linear decay)
 * IEC 60268-10 Type I approximation
 */
static void apply_ppm_ballistics(float current_db, float *ppm_db,
                                  uint32_t *last_update_time, bool *overload) {
    uint32_t now = esp_timer_get_time() / 1000; // milliseconds
    float dt = (now - *last_update_time) / 1000.0f; // seconds
    *last_update_time = now;
    
    // Clamp dt to reasonable range (handle initialization and overflow)
    if (dt > 1.0f || dt < 0.0f) dt = 0.02f; // Default to 20ms if suspicious
    
    if (current_db > *ppm_db) {
        // Fast attack with 10ms integration time constant
        // alpha = 1 - exp(-dt/tau), where tau = 0.01s
        float alpha = 1.0f - expf(-dt / 0.01f);
        *ppm_db = *ppm_db + (current_db - *ppm_db) * alpha;
    } else {
        // Linear decay at 7dB/sec
        *ppm_db -= PCM_VIZ_PPM_DECAY_DB_PER_SEC * dt;
        
        // Don't decay below current level
        if (*ppm_db < current_db) {
            *ppm_db = current_db;
        }
    }
    
    // Clamp to PPM scale range
    if (*ppm_db < PCM_VIZ_PPM_MIN_DB) *ppm_db = PCM_VIZ_PPM_MIN_DB;
    if (*ppm_db > PCM_VIZ_PPM_MAX_DB) *ppm_db = PCM_VIZ_PPM_MAX_DB;
    
    // Extra safety clamp
    *ppm_db = fmaxf(PCM_VIZ_PPM_MIN_DB, fminf(*ppm_db, PCM_VIZ_PPM_MAX_DB));
    
    // Overload detection
    *overload = (*ppm_db >= PCM_VIZ_PPM_OVERLOAD_DB);
}

static void calculate_rms_loudness(void) {
    // Calculate RMS for left channel
    float sum_sq_left = 0.0f;
    for (uint32_t i = 0; i < PCM_VIZ_WINDOW_SIZE; i++) {
        float sample = samples_left[i] * viz_gain;  // Remove auto-gain multiplication
        sum_sq_left += sample * sample;
    }
    float rms_left = sqrtf(sum_sq_left / PCM_VIZ_WINDOW_SIZE);

    // Calculate RMS for right channel
    float sum_sq_right = 0.0f;
    for (uint32_t i = 0; i < PCM_VIZ_WINDOW_SIZE; i++) {
        float sample = samples_right[i] * viz_gain;  // Remove auto-gain multiplication
        sum_sq_right += sample * sample;
    }
    float rms_right = sqrtf(sum_sq_right / PCM_VIZ_WINDOW_SIZE);

    // Convert to dB
    float left_db = rms_to_db(rms_left);
    float right_db = rms_to_db(rms_right);

    // Take mutex for loudness update
    if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
        // Apply PPM ballistics to both channels
        apply_ppm_ballistics(left_db,
                            &current_loudness.left_db,
                            &current_loudness.left_peak_time,
                            &current_loudness.left_overload);
        
        apply_ppm_ballistics(right_db,
                            &current_loudness.right_db,
                            &current_loudness.right_peak_time,
                            &current_loudness.right_overload);
        
        // Convert PPM dB to normalized 0-1 range for fixed scale
        float left_normalized = (current_loudness.left_db - PCM_VIZ_PPM_MIN_DB) / PCM_VIZ_PPM_DB_RANGE;
        float right_normalized = (current_loudness.right_db - PCM_VIZ_PPM_MIN_DB) / PCM_VIZ_PPM_DB_RANGE;
        
        // Clamp to valid range
        current_loudness.left = fminf(fmaxf(left_normalized, 0.0f), 1.0f);
        current_loudness.right = fminf(fmaxf(right_normalized, 0.0f), 1.0f);
        
        // For compatibility, also set peak fields (using PPM value as "peak")
        current_loudness.left_peak_db = current_loudness.left_db;
        current_loudness.right_peak_db = current_loudness.right_db;
        current_loudness.left_peak = current_loudness.left;
        current_loudness.right_peak = current_loudness.right;
        
        // Disable auto-gain for PPM meter (was causing meter to stay at full scale)
        current_loudness.auto_gain = 1.0f;  // Fixed gain
        s_auto_gain = 1.0f;
        // Do NOT call update_auto_gain() - it was multiplying the signal excessively
        
        // Debug logging
        #if CONFIG_LOG_DEFAULT_LEVEL >= 4
        static uint32_t log_counter = 0;
        if (++log_counter % 50 == 0) {
            ESP_LOGD(TAG, "PPM: L=%.1fdB(%.2f) R=%.1fdB(%.2f) | OL: L=%d R=%d | Gain=%.2f",
                current_loudness.left_db, current_loudness.left,
                current_loudness.right_db, current_loudness.right,
                current_loudness.left_overload, current_loudness.right_overload,
                s_auto_gain);
        }
        #endif
        
        xSemaphoreGive(data_mutex);
    }
}

static void pcm_viz_task(void* param) {
    ESP_LOGI(TAG, "Visualizer task started");

    while (true) {
        // Process available data
        process_ring_buffer();

        // Update buffer level stat
        viz_stats.buffer_level = get_buffer_bytes_available();

        // Small delay to prevent hogging CPU (reduced from 5ms to 2ms for faster updates)
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t pcm_viz_get_loudness(pcm_viz_loudness_t* loudness) {
    if (!is_initialized || loudness == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(loudness, &current_loudness, sizeof(pcm_viz_loudness_t));
        xSemaphoreGive(data_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t pcm_viz_get_stats(pcm_viz_stats_t* stats) {
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(stats, &viz_stats, sizeof(pcm_viz_stats_t));
    return ESP_OK;
}

size_t pcm_viz_get_buffer_level(void) {
    return get_buffer_bytes_available();
}

void pcm_viz_clear_buffers(void) {
    if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
        write_ptr = 0;
        read_ptr = 0;
        sample_index = 0;
        memset(samples_left, 0, sizeof(samples_left));
        memset(samples_right, 0, sizeof(samples_right));
        memset(&current_loudness, 0, sizeof(current_loudness));
        xSemaphoreGive(data_mutex);
    }
}

void pcm_viz_set_gain(float gain) {
    viz_gain = gain;
}