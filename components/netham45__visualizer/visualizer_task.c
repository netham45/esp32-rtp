#include "visualizer_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "pcm_visualizer.h"
#include "led_strip_controller.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "visualizer_task";

// Task handle
static TaskHandle_t visualizer_task_handle = NULL;
static bool task_running = false;

// LED controller instance
static led_controller_t* led_controller = NULL;

// Configuration
static led_color_scheme_t color_scheme = LED_COLOR_HEAT;
static float smoothing = 0.7f;
static volatile bool s_viz_suspended = false;
static volatile bool s_rendering = false;

// Main visualizer task
static void visualizer_task(void* param) {
    ESP_LOGI(TAG, "LED Visualizer task started");

    // Buffer for loudness data
    pcm_viz_loudness_t loudness;

    // LED update period for 60 FPS
    const TickType_t update_period = pdMS_TO_TICKS(16); // ~60Hz
    TickType_t last_wake_time = xTaskGetTickCount();

    while (task_running) {
        // When suspended (e.g., during NVS flash commits when cache is disabled), skip LED updates
        if (s_viz_suspended) {
            s_rendering = false;
            vTaskDelayUntil(&last_wake_time, update_period);
            continue;
        }

        // Get loudness values from PCM visualizer
        s_rendering = true;
        esp_err_t ret = pcm_viz_get_loudness(&loudness);

        if (ret == ESP_OK) {
            // Update LED strip with loudness data (using extended function with peak hold)
            led_controller_update_from_loudness_ex(led_controller, &loudness);
            led_controller_render(led_controller);
        }
        s_rendering = false;

        // Wait until next update period
        vTaskDelayUntil(&last_wake_time, update_period);
    }

    ESP_LOGI(TAG, "LED Visualizer task stopped");
    vTaskDelete(NULL);
}

// Initialize visualizer with LED strip
esp_err_t visualizer_init(void) {
    ESP_LOGI(TAG, "Initializing LED visualizer");

    // Initialize PCM visualizer if not already done
    esp_err_t ret = pcm_viz_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize PCM visualizer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Allocate and initialize LED strip controller
    led_controller = calloc(1, sizeof(led_controller_t));
    if (led_controller == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LED controller");
        pcm_viz_deinit();
        return ESP_FAIL;
    }

    // Configure LED strip
    // Disable RMT DMA to avoid ISR-driven encoding while flash cache can be disabled (e.g. during NVS reads)
    visualizer_led_config_t led_config = {
        .gpio_pin = 48,  // Default GPIO for LED strip
        .num_leds = LED_STRIP_NUM_LEDS,
        .brightness = 128,
        .enable_dma = false,  // critical: avoid dma_tx_eof ISR path calling encoder when cache may be off
        .color_scheme = color_scheme,
        .smoothing_factor = smoothing
    };

    ret = led_controller_init(led_controller, &led_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip controller: %s", esp_err_to_name(ret));
        free(led_controller);
        led_controller = NULL;
        pcm_viz_deinit();
        return ret;
    }

    // Play power-on animation
    ret = led_controller_power_on_animation(led_controller);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Power-on animation failed: %s", esp_err_to_name(ret));
        // Don't fail initialization if animation fails
    }

    // Create visualizer task
    task_running = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        visualizer_task,
        "led_viz",
        8192,           // Stack size
        NULL,           // Parameters
        3,              // Priority (lower than audio tasks)
        &visualizer_task_handle,
        0               // Core 0
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create visualizer task %d", task_ret);
        led_controller_deinit(led_controller);
        free(led_controller);
        led_controller = NULL;
        pcm_viz_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LED visualizer initialized successfully");
    return ESP_OK;
}

// Deinitialize visualizer
esp_err_t visualizer_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing LED visualizer");

    // Stop task
    task_running = false;
    if (visualizer_task_handle != NULL) {
        // Wait a bit for task to stop
        vTaskDelay(pdMS_TO_TICKS(50));
        visualizer_task_handle = NULL;
    }

    // Deinitialize LED controller
    if (led_controller != NULL) {
        led_controller_deinit(led_controller);
        free(led_controller);
        led_controller = NULL;
    }

    // Deinitialize PCM visualizer
    pcm_viz_deinit();

    ESP_LOGI(TAG, "LED visualizer deinitialized");
    return ESP_OK;
}

// Set color scheme
esp_err_t visualizer_set_color_scheme(led_color_scheme_t scheme) {
    if (led_controller == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    color_scheme = scheme;
    led_controller_set_color_scheme(led_controller, scheme);
    return ESP_OK;
}

// Set smoothing factor
esp_err_t visualizer_set_smoothing(float factor) {
    if (led_controller == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (factor < 0.0f || factor > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    smoothing = factor;
    led_controller->config.smoothing_factor = factor;
    return ESP_OK;
}

// Enable/disable peak hold (deprecated for loudness visualizer)
esp_err_t visualizer_set_peak_hold(bool enable) {
    // Peak hold is handled by the PCM visualizer for loudness mode
    // This function is kept for API compatibility but does nothing
    return ESP_OK;
}

// Set visualization gain
esp_err_t visualizer_set_gain(float gain) {
    pcm_viz_set_gain(gain);
    return ESP_OK;
}

// Feed PCM data to visualizer
esp_err_t visualizer_feed_pcm(const uint8_t* data, size_t len) {
    return pcm_viz_write(data, len);
}

// Get visualizer statistics
esp_err_t visualizer_get_stats(visualizer_stats_t* stats) {
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get PCM visualizer stats
    pcm_viz_stats_t pcm_stats;
    esp_err_t ret = pcm_viz_get_stats(&pcm_stats);
    if (ret != ESP_OK) {
        return ret;
    }

    // Fill in stats
    stats->samples_processed = pcm_stats.samples_processed;
    stats->fft_runs = 0;  // No FFT in loudness mode
    stats->buffer_level = pcm_stats.buffer_level;
    stats->buffer_overruns = pcm_stats.buffer_overruns;
    stats->task_running = task_running;

    // LED stats not implemented yet, set to 0
    stats->led_updates = 0;
    stats->led_errors = 0;

    return ESP_OK;
}

// Runtime suspend/resume helpers to avoid LED/RMT calls while flash cache is disabled.
// Safe to call regardless of visualizer init state; the task loop checks this flag.
esp_err_t visualizer_suspend(void) {
    s_viz_suspended = true;
    // Wait briefly for any in-flight render to finish to avoid cache-disabled refresh
    for (int i = 0; i < 5 && s_rendering; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

esp_err_t visualizer_resume(void) {
    s_viz_suspended = false;
    return ESP_OK;
}
/**
 * Report whether the visualizer is currently active (task running and controller allocated).
 * Used by config manager to temporarily stop/restart LEDs around flash commits.
 */
bool visualizer_is_active(void) {
    return task_running && (led_controller != NULL);
}