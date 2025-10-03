#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "led_strip.h"

// LED strip physical layout configuration
#define LED_STRIP_NUM_LEDS      129     // Total number of LEDs (128 + 1 to skip LED #0)
#define LED_STRIP_NUM_COLUMNS   64      // Number of columns
#define LED_STRIP_NUM_ROWS      2       // Number of rows
#define LED_STRIP_LEFT_COLS     32      // Left speaker columns (0-31)
#define LED_STRIP_RIGHT_COLS    32      // Right speaker columns (32-63)

// LED indexing:
// Physical layout: 64 columns × 2 rows = 128 LEDs
// LED #0 is skipped (ESP32 built-in LED)
// Left speaker:  columns 0-31, both rows (LEDs 1-64)
// Right speaker: columns 32-63, both rows (LEDs 65-128)
//
// LED index = column + (row * 64) + 1
// Example: Column 0, Row 0 = LED 1
//          Column 0, Row 1 = LED 65
//          Column 32, Row 0 = LED 33
//          Column 32, Row 1 = LED 97

// Color scheme types
typedef enum {
    LED_COLOR_RAINBOW = 0,
    LED_COLOR_HEAT,
    LED_COLOR_MONOCHROME,
    LED_COLOR_CUSTOM,
    LED_COLOR_MAX
} led_color_scheme_t;

// LED strip configuration
typedef struct {
    uint8_t gpio_pin;
    uint8_t num_leds;
    uint8_t brightness;
    bool enable_dma;
    led_color_scheme_t color_scheme;
    float smoothing_factor;
} visualizer_led_config_t;

// RGB color structure
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

// LED controller state
typedef struct {
    led_strip_handle_t strip;
    rgb_color_t frame_buffer[LED_STRIP_NUM_LEDS];
    float smoothed_left;   // Smoothed loudness for left channel
    float smoothed_right;  // Smoothed loudness for right channel
    visualizer_led_config_t config;
    bool initialized;
} led_controller_t;

/**
 * @brief Initialize the LED strip controller
 *
 * @param controller Pointer to controller structure
 * @param config Configuration parameters
 * @return ESP_OK on success
 */
esp_err_t led_controller_init(led_controller_t* controller, const visualizer_led_config_t* config);

/**
 * @brief Update LED visualization from loudness values
 *
 * @param controller Pointer to controller structure
 * @param left_loudness Left channel loudness (0.0-1.0)
 * @param right_loudness Right channel loudness (0.0-1.0)
 * @return ESP_OK on success
 */
esp_err_t led_controller_update_from_loudness(led_controller_t* controller,
                                               float left_loudness,
                                               float right_loudness);

/**
 * @brief Update LED visualization from loudness structure (with peak hold support)
 *
 * @param controller Pointer to controller structure
 * @param loudness_struct Pointer to pcm_viz_loudness_t structure
 * @return ESP_OK on success
 */
esp_err_t led_controller_update_from_loudness_ex(led_controller_t* controller,
                                                  const void* loudness_struct);

/**
 * @brief Render the current frame buffer to the LED strip
 *
 * @param controller Pointer to controller structure
 * @return ESP_OK on success
 */
esp_err_t led_controller_render(led_controller_t* controller);

/**
 * @brief Set brightness level
 *
 * @param controller Pointer to controller structure
 * @param brightness Brightness level (0-255)
 */
void led_controller_set_brightness(led_controller_t* controller, uint8_t brightness);

/**
 * @brief Set color scheme
 *
 * @param controller Pointer to controller structure
 * @param scheme Color scheme to use
 */
void led_controller_set_color_scheme(led_controller_t* controller, led_color_scheme_t scheme);

/**
 * @brief Clear all LEDs
 *
 * @param controller Pointer to controller structure
 * @return ESP_OK on success
 */
esp_err_t led_controller_clear(led_controller_t* controller);

/**
 * @brief Play a power-on animation (red sweep)
 *
 * @param controller Pointer to controller structure
 * @return ESP_OK on success
 */
esp_err_t led_controller_power_on_animation(led_controller_t* controller);

/**
 * @brief Deinitialize the LED strip controller
 *
 * @param controller Pointer to controller structure
 */
void led_controller_deinit(led_controller_t* controller);