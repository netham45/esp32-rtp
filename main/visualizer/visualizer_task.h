#ifndef VISUALIZER_TASK_H
#define VISUALIZER_TASK_H

#include "esp_err.h"
#include "led_strip_controller.h"
#include <stdbool.h>

#ifdef __cplusplus 
extern "C" {
#endif

// Visualizer statistics
typedef struct {
    uint32_t samples_processed;
    uint32_t fft_runs;
    uint32_t buffer_level;
    uint32_t buffer_overruns;
    uint32_t led_updates;
    uint32_t led_errors;
    bool task_running;
} visualizer_stats_t;

/**
 * Initialize the LED visualizer
 * 
 * This initializes both the PCM visualizer and LED strip controller,
 * and starts the visualization task.
 * 
 * @return ESP_OK on success
 */
esp_err_t visualizer_init(void);

/**
 * Deinitialize the LED visualizer
 * 
 * @return ESP_OK on success
 */
esp_err_t visualizer_deinit(void);

/**
 * Feed PCM audio data to the visualizer
 * 
 * @param data PCM audio data buffer
 * @param len Length of data in bytes
 * @return ESP_OK on success
 */
esp_err_t visualizer_feed_pcm(const uint8_t* data, size_t len);

/**
 * Set the color scheme for LED visualization
 * 
 * @param scheme Color scheme to use
 * @return ESP_OK on success
 */
esp_err_t visualizer_set_color_scheme(led_color_scheme_t scheme);

/**
 * Set the smoothing factor for LED transitions
 * 
 * @param factor Smoothing factor (0.0 = no smoothing, 1.0 = maximum)
 * @return ESP_OK on success
 */
esp_err_t visualizer_set_smoothing(float factor);

/**
 * Enable or disable peak hold effect
 * 
 * @param enable true to enable peak hold
 * @return ESP_OK on success
 */
esp_err_t visualizer_set_peak_hold(bool enable);

/**
 * Set the visualization gain
 * 
 * @param gain Gain factor (typical range: 0.5 to 2.0)
 * @return ESP_OK on success
 */
esp_err_t visualizer_set_gain(float gain);

/**
 * Get visualizer statistics
 * 
 * @param stats Pointer to statistics structure to fill
 * @return ESP_OK on success
 */
esp_err_t visualizer_get_stats(visualizer_stats_t* stats);

/**
 * Query whether the visualizer is currently active (initialized and running).
 */
bool visualizer_is_active(void);

/**
 * Temporarily suspend/resume LED updates safely.
 * Safe to call even if visualizer is not initialized.
 * Used to avoid calling RMT/LED code while flash cache is disabled (e.g., during NVS writes).
 */
esp_err_t visualizer_suspend(void);
esp_err_t visualizer_resume(void);

#ifdef __cplusplus
}
#endif

#endif // VISUALIZER_TASK_H