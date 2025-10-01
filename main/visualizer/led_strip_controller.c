#include "led_strip_controller.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char* TAG = "led_controller";

// Color calculation helpers
static rgb_color_t hsv_to_rgb(float h, float s, float v);
static rgb_color_t get_heat_color(float value);
static rgb_color_t get_rainbow_color(uint8_t position);
static rgb_color_t scale_color(rgb_color_t color, uint8_t brightness);

// Helper to get LED index for a given column and row
// LED #0 is skipped, so we add 1 to the index
static inline uint8_t get_led_index(uint8_t column, uint8_t row) {
    return column + (row * LED_STRIP_NUM_COLUMNS) + 1;
}

// Initialize the LED strip controller
esp_err_t led_controller_init(led_controller_t* controller, const visualizer_led_config_t* config) {
    if (controller == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing LED strip controller (GPIO %d, %d LEDs)",
             config->gpio_pin, config->num_leds);

    // Store configuration
    memcpy(&controller->config, config, sizeof(visualizer_led_config_t));

    // Configure LED strip
    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio_pin,
        .max_leds = config->num_leds,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
        .flags.with_dma = config->enable_dma,
    };

    // Create LED strip instance
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &controller->strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize buffers
    memset(controller->frame_buffer, 0, sizeof(controller->frame_buffer));
    controller->smoothed_left = 0.0f;
    controller->smoothed_right = 0.0f;

    // Clear the strip
    led_strip_clear(controller->strip);

    controller->initialized = true;
    ESP_LOGI(TAG, "LED strip controller initialized");

    return ESP_OK;
}

// Update LED visualization from loudness values (extended to support peak hold)
esp_err_t led_controller_update_from_loudness(led_controller_t* controller,
                                               float left_loudness,
                                               float right_loudness) {
    if (!controller->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Apply smoothing
    float smoothing = controller->config.smoothing_factor;
    controller->smoothed_left = (controller->smoothed_left * smoothing) +
                                (left_loudness * (1.0f - smoothing));
    controller->smoothed_right = (controller->smoothed_right * smoothing) +
                                 (right_loudness * (1.0f - smoothing));

    // Calculate number of columns to light up (0-32 for each channel)
    uint8_t left_columns = (uint8_t)(controller->smoothed_left * LED_STRIP_LEFT_COLS);
    uint8_t right_columns = (uint8_t)(controller->smoothed_right * LED_STRIP_RIGHT_COLS);

    // Clear frame buffer
    memset(controller->frame_buffer, 0, sizeof(controller->frame_buffer));

    // Light up left speaker columns (0-31)
    for (uint8_t col = 0; col < LED_STRIP_LEFT_COLS; col++) {
        if (col < left_columns) {
            // Calculate color based on position and scheme
            rgb_color_t color;

            switch (controller->config.color_scheme) {
                case LED_COLOR_RAINBOW:
                    // Rainbow colors across the columns
                    color = get_rainbow_color(col * 8);
                    break;

                case LED_COLOR_HEAT:
                    // Heat colors: dark blue -> purple -> red -> yellow as loudness increases
                    color = get_heat_color((float)col / LED_STRIP_LEFT_COLS);
                    break;

                case LED_COLOR_MONOCHROME:
                default:
                    // Green monochrome
                    color = (rgb_color_t){0, 255, 0};
                    break;
            }

            // Apply brightness
            color = scale_color(color, controller->config.brightness);

            // Set both LEDs in this column (both rows)
            controller->frame_buffer[get_led_index(col, 0)] = color;
            controller->frame_buffer[get_led_index(col, 1)] = color;
        }
    }

    // Light up right speaker columns (32-63)
    for (uint8_t col = 0; col < LED_STRIP_RIGHT_COLS; col++) {
        if (col < right_columns) {
            // Calculate actual column index (32-63)
            uint8_t actual_col = col + LED_STRIP_LEFT_COLS;

            // Calculate color based on position and scheme
            rgb_color_t color;

            switch (controller->config.color_scheme) {
                case LED_COLOR_RAINBOW:
                    // Rainbow colors across the columns
                    color = get_rainbow_color(col * 8);
                    break;

                case LED_COLOR_HEAT:
                    // Heat colors: dark blue -> purple -> red -> yellow as loudness increases
                    color = get_heat_color((float)col / LED_STRIP_RIGHT_COLS);
                    break;

                case LED_COLOR_MONOCHROME:
                default:
                    // Green monochrome
                    color = (rgb_color_t){0, 255, 0};
                    break;
            }

            // Apply brightness
            color = scale_color(color, controller->config.brightness);

            // Set both LEDs in this column (both rows)
            controller->frame_buffer[get_led_index(actual_col, 0)] = color;
            controller->frame_buffer[get_led_index(actual_col, 1)] = color;
        }
    }

    return ESP_OK;
}

// Update LED visualization from loudness structure (with peak hold support)
esp_err_t led_controller_update_from_loudness_ex(led_controller_t* controller,
                                                  const void* loudness_struct) {
    if (!controller->initialized || !loudness_struct) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Cast loudness structure
    typedef struct {
        float left, right;
        float left_db, right_db;
        float left_peak, right_peak;
        float left_peak_db, right_peak_db;
        uint32_t left_peak_time, right_peak_time;
        bool left_overload, right_overload;
        float auto_gain;
    } loudness_data_t;
    
    const loudness_data_t* loudness = (const loudness_data_t*)loudness_struct;
    
    // No smoothing - use direct PPM values
    
    // Clear frame buffer
    memset(controller->frame_buffer, 0, sizeof(controller->frame_buffer));
    
    // === LEFT CHANNEL ===
    // PPM value already normalized to 0-1 for fixed scale
    uint8_t left_lit_cols = (uint8_t)(loudness->left * LED_STRIP_LEFT_COLS);

    // Draw left bar with color zones - expanding from CENTER (col 31) to LEFT (col 0)
    for (uint8_t col = 0; col < left_lit_cols; col++) {
        // Reverse the column index so it grows from right to left (from center outwards)
        uint8_t actual_col = (LED_STRIP_LEFT_COLS - 1) - col;

        float intensity = (float)col / LED_STRIP_LEFT_COLS;

        // Calculate dB at this LED position
        float db_at_led = -40.0f + (intensity * 40.0f); // -40 to 0dB

        rgb_color_t color;

        if (db_at_led >= -3.0f) {
            // Overload warning zone (-3 to 0dB): BRIGHT RED
            color = (rgb_color_t){255, 0, 0};
        } else if (db_at_led >= -12.0f) {
            // Warning zone (-12 to -3dB): YELLOW/ORANGE
            float warn_intensity = (db_at_led + 12.0f) / 9.0f; // 0-1 in warning zone
            color = (rgb_color_t){
                255,
                (uint8_t)(200 - (warn_intensity * 100)), // Yellow→Orange
                0
            };
        } else {
            // Normal zone (-40 to -12dB): Use heat gradient
            float normal_intensity = (db_at_led + 40.0f) / 28.0f; // 0-1 in normal zone
            color = get_heat_color(normal_intensity);
        }

        // Apply brightness
        color = scale_color(color, controller->config.brightness);

        // Set both rows
        controller->frame_buffer[get_led_index(actual_col, 0)] = color;
        controller->frame_buffer[get_led_index(actual_col, 1)] = color;
    }
    
    // === RIGHT CHANNEL ===
    uint8_t right_lit_cols = (uint8_t)(loudness->right * LED_STRIP_RIGHT_COLS);
    
    // Draw right bar with color zones
    for (uint8_t col = 0; col < right_lit_cols; col++) {
        uint8_t actual_col = col + LED_STRIP_LEFT_COLS;
        float intensity = (float)col / LED_STRIP_RIGHT_COLS;
        
        // Calculate dB at this LED position
        float db_at_led = -40.0f + (intensity * 40.0f); // -40 to 0dB
        
        rgb_color_t color;
        
        if (db_at_led >= -3.0f) {
            // Overload warning zone (-3 to 0dB): BRIGHT RED
            color = (rgb_color_t){255, 0, 0};
        } else if (db_at_led >= -12.0f) {
            // Warning zone (-12 to -3dB): YELLOW/ORANGE
            float warn_intensity = (db_at_led + 12.0f) / 9.0f;
            color = (rgb_color_t){
                255,
                (uint8_t)(200 - (warn_intensity * 100)),
                0
            };
        } else {
            // Normal zone (-40 to -12dB): Heat gradient
            float normal_intensity = (db_at_led + 40.0f) / 28.0f;
            color = get_heat_color(normal_intensity);
        }
        
        // Apply brightness
        color = scale_color(color, controller->config.brightness);
        
        // Set both rows
        controller->frame_buffer[get_led_index(actual_col, 0)] = color;
        controller->frame_buffer[get_led_index(actual_col, 1)] = color;
    }
    
    return ESP_OK;
}

// Render the current frame buffer to the LED strip
esp_err_t led_controller_render(led_controller_t* controller) {
    if (!controller->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Update each LED
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        led_strip_set_pixel(controller->strip, i,
                           controller->frame_buffer[i].r,
                           controller->frame_buffer[i].g,
                           controller->frame_buffer[i].b);
    }

    // Refresh the strip
    return led_strip_refresh(controller->strip);
}

// Set brightness level
void led_controller_set_brightness(led_controller_t* controller, uint8_t brightness) {
    if (controller != NULL) {
        controller->config.brightness = brightness;
    }
}

// Set color scheme
void led_controller_set_color_scheme(led_controller_t* controller, led_color_scheme_t scheme) {
    if (controller != NULL && scheme < LED_COLOR_MAX) {
        controller->config.color_scheme = scheme;
    }
}

// Clear all LEDs
esp_err_t led_controller_clear(led_controller_t* controller) {
    if (!controller->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clear frame buffer
    memset(controller->frame_buffer, 0, sizeof(controller->frame_buffer));

    // Clear the physical strip
    led_strip_clear(controller->strip);

    return ESP_OK;
}

// Play a power-on animation (red sweep)
esp_err_t led_controller_power_on_animation(led_controller_t* controller) {
    if (!controller->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing power-on animation");

    // Clear all LEDs first
    led_controller_clear(controller);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Red color at full brightness
    rgb_color_t red = {255, 0, 0};
    rgb_color_t dimmed_red = {64, 0, 0};
    rgb_color_t off = {0, 0, 0};

    // Sweep left to right
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        // Clear previous LEDs
        memset(controller->frame_buffer, 0, sizeof(controller->frame_buffer));

        // Set current LED bright red with trailing tail
        controller->frame_buffer[i] = red;
        if (i > 0) {
            controller->frame_buffer[i - 1] = dimmed_red;
        }
        if (i > 1) {
            controller->frame_buffer[i - 2] = off;
        }

        led_controller_render(controller);
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // Brief pause at the end
    vTaskDelay(pdMS_TO_TICKS(50));

    // Sweep right to left
    for (int i = LED_STRIP_NUM_LEDS - 1; i >= 0; i--) {
        // Clear previous LEDs
        memset(controller->frame_buffer, 0, sizeof(controller->frame_buffer));

        // Set current LED bright red with trailing tail
        controller->frame_buffer[i] = red;
        if (i < LED_STRIP_NUM_LEDS - 1) {
            controller->frame_buffer[i + 1] = dimmed_red;
        }
        if (i < LED_STRIP_NUM_LEDS - 2) {
            controller->frame_buffer[i + 2] = off;
        }

        led_controller_render(controller);
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // Clear all LEDs at the end
    vTaskDelay(pdMS_TO_TICKS(100));
    led_controller_clear(controller);

    ESP_LOGI(TAG, "Power-on animation complete");
    return ESP_OK;
}

// Deinitialize the LED strip controller
void led_controller_deinit(led_controller_t* controller) {
    if (controller != NULL && controller->initialized) {
        led_controller_clear(controller);
        led_strip_del(controller->strip);
        controller->initialized = false;
        ESP_LOGI(TAG, "LED strip controller deinitialized");
    }
}

// Color helper functions

static rgb_color_t hsv_to_rgb(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float r, g, b;

    if (h >= 0 && h < 60) {
        r = c; g = x; b = 0;
    } else if (h >= 60 && h < 120) {
        r = x; g = c; b = 0;
    } else if (h >= 120 && h < 180) {
        r = 0; g = c; b = x;
    } else if (h >= 180 && h < 240) {
        r = 0; g = x; b = c;
    } else if (h >= 240 && h < 300) {
        r = x; g = 0; b = c;
    } else {
        r = c; g = 0; b = x;
    }

    return (rgb_color_t){
        .r = (uint8_t)((r + m) * 255),
        .g = (uint8_t)((g + m) * 255),
        .b = (uint8_t)((b + m) * 255)
    };
}

static rgb_color_t get_rainbow_color(uint8_t position) {
    float hue = (position * 360.0f) / 256.0f;
    return hsv_to_rgb(hue, 1.0f, 1.0f);
}

static rgb_color_t get_heat_color(float value) {
    // Map value (0-1) to heat colors: dark blue -> purple -> red -> yellow
    value = fminf(fmaxf(value, 0.0f), 1.0f);

    uint8_t r, g, b;

    if (value < 0.33f) {
        // Dark blue to purple (0.0 - 0.33)
        float t = value / 0.33f;
        r = (uint8_t)(t * 128);      // Red increases to 128
        g = 0;
        b = (uint8_t)(64 + t * 64);  // Blue: 64 -> 128
    } else if (value < 0.66f) {
        // Purple to red (0.33 - 0.66)
        float t = (value - 0.33f) / 0.33f;
        r = (uint8_t)(128 + t * 127); // Red: 128 -> 255
        g = 0;
        b = (uint8_t)(128 * (1.0f - t)); // Blue: 128 -> 0
    } else {
        // Red to yellow (0.66 - 1.0)
        float t = (value - 0.66f) / 0.34f;
        r = 255;
        g = (uint8_t)(t * 200);      // Green increases to 200 (dimmer yellow)
        b = 0;
    }

    return (rgb_color_t){r, g, b};
}

static rgb_color_t scale_color(rgb_color_t color, uint8_t brightness) {
    return (rgb_color_t){
        .r = (color.r * brightness) / 255,
        .g = (color.g * brightness) / 255,
        .b = (color.b * brightness) / 255
    };
}