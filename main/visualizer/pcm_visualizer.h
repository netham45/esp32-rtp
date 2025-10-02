#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "build_config.h"

// Configuration constants (sourced from Kconfig via build_config.h)
#define PCM_VIZ_CHUNK_SIZE      CONFIG_VIZ_CHUNK_SIZE
#define PCM_VIZ_RING_SIZE       CONFIG_VIZ_RING_SIZE
#define PCM_VIZ_WINDOW_SIZE     CONFIG_VIZ_WINDOW_SIZE    // Sample window for RMS calculation (21.3ms at 48kHz)

// VU Meter Ballistics (fixed-point -> float)
#define PCM_VIZ_ATTACK_COEFF  (CONFIG_VIZ_ATTACK_COEFF_MPCT / 1000.0f)   // Fast attack (~50ms)
#define PCM_VIZ_RELEASE_COEFF (CONFIG_VIZ_RELEASE_COEFF_MPCT / 1000.0f)  // Slow release (~300ms)

// dB Range Configuration (for PPM scale)
#define PCM_VIZ_MIN_DB PCM_VIZ_PPM_MIN_DB
#define PCM_VIZ_MAX_DB PCM_VIZ_PPM_MAX_DB
#define PCM_VIZ_DB_RANGE PCM_VIZ_PPM_DB_RANGE

// Peak Hold Configuration
#define PCM_VIZ_PEAK_HOLD_MS CONFIG_VIZ_PEAK_HOLD_MS    // 1.5 seconds
#define PCM_VIZ_PEAK_DECAY_DB_PER_SEC 20.0f

// Auto-Gain Configuration (fixed-point -> float)
#define PCM_VIZ_AUTO_GAIN_MIN (CONFIG_VIZ_AUTO_GAIN_MIN_MPCT / 1000.0f)
#define PCM_VIZ_AUTO_GAIN_MAX (CONFIG_VIZ_AUTO_GAIN_MAX_MPCT / 1000.0f)
#define PCM_VIZ_AUTO_GAIN_TARGET (CONFIG_VIZ_AUTO_GAIN_TARGET_MPCT / 1000.0f)    // Target peak at 80% (26/32 LEDs)
#define PCM_VIZ_AUTO_GAIN_ADJUST_RATE (CONFIG_VIZ_AUTO_GAIN_ADJUST_RATE_MPCT / 1000.0f)  // ±5% per second max
#define PCM_VIZ_AUTO_GAIN_WINDOW_MS CONFIG_VIZ_AUTO_GAIN_WINDOW_MS     // 1-second window

// PPM Meter Configuration (IEC 60268-10 Type I style)
#define PCM_VIZ_PPM_INTEGRATION_MS 10        // 10ms quasi-peak integration
#define PCM_VIZ_PPM_DECAY_DB_PER_SEC (CONFIG_VIZ_PPM_DECAY_DB_PER_SEC_TENTHS / 10.0f)
#define PCM_VIZ_PPM_MIN_DB (CONFIG_VIZ_PPM_MIN_DB_TENTHS / 10.0f)            // Scale minimum
#define PCM_VIZ_PPM_MAX_DB (CONFIG_VIZ_PPM_MAX_DB_TENTHS / 10.0f)            // Scale maximum
#define PCM_VIZ_PPM_DB_RANGE (CONFIG_VIZ_PPM_DB_RANGE_TENTHS / 10.0f)        // Total scale range
#define PCM_VIZ_PPM_OVERLOAD_DB (CONFIG_VIZ_PPM_OVERLOAD_DB_TENTHS / 10.0f)  // Overload warning threshold

// Smoothing (reduced from 0.7)
#define PCM_VIZ_SMOOTH_COEFF (CONFIG_VIZ_SMOOTH_COEFF_MPCT / 1000.0f)

// PPM Loudness Analysis
typedef struct {
    float left;                  // Left channel PPM level (0.0-1.0)
    float right;                 // Right channel PPM level (0.0-1.0)
    float left_db;               // Left channel in dB (for reference)
    float right_db;              // Right channel in dB (for reference)
    float left_peak_db;          // Peak hold in dB (legacy compatibility)
    float right_peak_db;         // Peak hold in dB (legacy compatibility)
    uint32_t left_peak_time;     // Timestamp of last left peak
    uint32_t right_peak_time;    // Timestamp of last right peak
    bool left_overload;          // Left channel overload flag (>= 0dB)
    bool right_overload;         // Right channel overload flag (>= 0dB)
    float auto_gain;             // Auto-gain multiplier
    float left_peak;             // DEPRECATED: kept for compatibility
    float right_peak;            // DEPRECATED: kept for compatibility
} pcm_viz_loudness_t;

// Visualizer statistics
typedef struct {
    uint32_t samples_processed;
    uint32_t buffer_overruns;
    size_t buffer_level;
} pcm_viz_stats_t;

/**
 * @brief Initialize the PCM visualizer module
 *
 * Sets up the ring buffer and processing task for loudness calculation.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t pcm_viz_init(void);

/**
 * @brief Deinitialize the PCM visualizer module
 *
 * Stops the processing task and frees resources.
 *
 * @return ESP_OK on success
 */
esp_err_t pcm_viz_deinit(void);

/**
 * @brief Write PCM data to the visualizer ring buffer
 *
 * This function is thread-safe and can be called from any context.
 * Data is 16-bit stereo PCM at 48kHz.
 *
 * @param data Pointer to PCM data
 * @param len Length of data in bytes
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer is full
 */
esp_err_t pcm_viz_write(const uint8_t* data, size_t len);

/**
 * @brief Get the current loudness values for left and right channels
 *
 * Returns normalized loudness values (0.0 to 1.0) based on RMS calculation.
 *
 * @param loudness Pointer to loudness structure to fill
 * @return ESP_OK on success
 */
esp_err_t pcm_viz_get_loudness(pcm_viz_loudness_t* loudness);

/**
 * @brief Get visualizer statistics
 *
 * @param stats Pointer to statistics structure
 * @return ESP_OK on success
 */
esp_err_t pcm_viz_get_stats(pcm_viz_stats_t* stats);

/**
 * @brief Get current ring buffer fill level
 *
 * @return Number of bytes currently in the buffer
 */
size_t pcm_viz_get_buffer_level(void);

/**
 * @brief Clear/reset the visualizer buffers
 */
void pcm_viz_clear_buffers(void);

/**
 * @brief Set gain for visualization
 *
 * @param gain Gain multiplier (1.0 = normal, 2.0 = double, etc.)
 */
void pcm_viz_set_gain(float gain);