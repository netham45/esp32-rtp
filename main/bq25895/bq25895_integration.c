/**
 * @file bq25895_integration.c
 * @brief Implementation of BQ25895 integration
 */

#include "bq25895_integration.h"
#include "bq25895/bq25895.h"
#include "bq25895/bq25895_web.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bq25895_integration";

// GPIO configuration for BQ25895
#define BQ25895_CE_PIN              12      // GPIO for CE pin
#define BQ25895_OTG_PIN             13      // GPIO for OTG pin

// Default charge parameters
static const bq25895_charge_params_t default_charge_params = {
    .charge_voltage_mv = 4208,         // 4.208V
    .charge_current_ma = 1024,         // 512mA
    .input_current_limit_ma = 1500,    // 1.5A
    .input_voltage_limit_mv = 4400,    // 4.4V
    .boost_voltage_mv = 4998,          // 5.126V
    .precharge_current_ma = 128,       // 128mA
    .termination_current_ma = 256,     // 256mA
    .enable_termination = true,
    .enable_charging = true,
    .enable_otg = true,
    .thermal_regulation_threshold = 3, // 120°C
    .fast_charge_timer_hours = 12,
    .enable_safety_timer = true,
    .enable_hi_impedance = false,
    .enable_ir_compensation = false,
    .ir_compensation_mohm = 0,
    .ir_compensation_voltage_mv = 0
};

// I2C master initialization is now handled by the BQ25895 driver

// Task to periodically reset the watchdog timer
static void watchdog_reset_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Watchdog reset task started");
    
    while (1) {
        // Reset the watchdog timer every 30 seconds
        // The BQ25895 watchdog timer is typically 40 seconds
        vTaskDelay(pdMS_TO_TICKS(30000));
        
        esp_err_t ret = bq25895_reset_watchdog();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to reset watchdog timer: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGD(TAG, "Watchdog timer reset successfully");
        }
        
        // Also ensure OTG mode is still enabled
        ret = bq25895_enable_otg(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to re-enable OTG mode: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t bq25895_integration_init(void)
{
    esp_err_t ret;
    
    // Initialize CE pin as output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BQ25895_CE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ret = gpio_config(&io_conf);
    io_conf.pin_bit_mask = (1ULL << BQ25895_OTG_PIN);
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure CE pin: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set CE pin low by default (enable charging)
    gpio_set_level(BQ25895_CE_PIN, 0);

    // Set OTG pin high by default (enable usb port power)
    gpio_set_level(BQ25895_OTG_PIN, 1);
    
    // Initialize BQ25895 driver with I2C configuration
    bq25895_config_t config = {
        .i2c_port = BQ25895_DEFAULT_I2C_PORT,  // Now an integer port number
        .i2c_freq = BQ25895_DEFAULT_I2C_FREQ_HZ,
        .sda_gpio = BQ25895_DEFAULT_SDA_GPIO,
        .scl_gpio = BQ25895_DEFAULT_SCL_GPIO,
        .int_gpio = -1,  // Not used
        .stat_gpio = -1  // Not used
    };
    ret = bq25895_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BQ25895 driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Explicitly enable OTG mode first to ensure PIMD 5V output is available
    ret = bq25895_enable_otg(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable OTG mode: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set boost voltage to 5V
    ret = bq25895_set_boost_voltage(4998);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boost voltage: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "PIMD 5V output explicitly enabled for DAC power");
    
    // Reset watchdog timer to prevent timeout
    ret = bq25895_reset_watchdog();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset watchdog timer: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "BQ25895 watchdog timer reset");
    
    ESP_LOGI(TAG, "Watchdog reset task started");
    
    // Add a small delay before starting the watchdog task to ensure initialization is complete
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Create a periodic task to reset the watchdog timer
    static TaskHandle_t watchdog_reset_task_handle = NULL;
    if (watchdog_reset_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            watchdog_reset_task,
            "watchdog_reset",
            2048,
            NULL,
            1,  // Low priority
            &watchdog_reset_task_handle,
            0   // Core 0
        );
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create watchdog reset task");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Watchdog reset task created");
    }

    // Enable ADC conversions for battery monitoring
    // Add delay before writing to ensure driver is ready
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = bq25895_write_reg(BQ25895_REG_02, 0x40); // Enable refreshing battery charge level otherwise it never polls
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable ADC conversions: %s", esp_err_to_name(ret));
    }
    
    // Configure watchdog settings
    uint8_t value = 0;
    ret = bq25895_read_reg(BQ25895_REG_07, &value);
    if (ret == ESP_OK) {
        value &= 0xE7;  // Clear watchdog timer bits
        ret = bq25895_write_reg(BQ25895_REG_07, value); // Disable watchdog (mcu doesn't implement any safety bits anyways)
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure watchdog settings: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Failed to read watchdog settings: %s", esp_err_to_name(ret));
    }
    
    // Set default charge parameters
    ret = bq25895_set_charge_params(&default_charge_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set default charge parameters: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize BQ25895 web interface
    ret = bq25895_web_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BQ25895 web interface: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "BQ25895 integration initialized successfully");
    return ESP_OK;
}

esp_err_t bq25895_integration_get_status(bq25895_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return bq25895_get_status(status);
}

esp_err_t bq25895_integration_get_charge_params(bq25895_charge_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return bq25895_get_charge_params(params);
}

esp_err_t bq25895_integration_set_charge_params(const bq25895_charge_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return bq25895_set_charge_params(params);
}

esp_err_t bq25895_integration_reset(void)
{
    esp_err_t ret = bq25895_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset BQ25895: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // After reset, set default parameters again
    ret = bq25895_set_charge_params(&default_charge_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set default charge parameters after reset: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "BQ25895 reset successfully");
    return ESP_OK;
}

esp_err_t bq25895_integration_set_ce_pin(bool enable)
{
    // CE pin is active low, so we need to invert the logic
    // When enable is true, we want to set CE pin low to enable charging
    // When enable is false, we want to set CE pin high to disable charging
    int level = enable ? 0 : 1;
    
    esp_err_t ret = gpio_set_level(BQ25895_CE_PIN, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CE pin level: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "BQ25895 CE pin set to %d (charging %s)", level, enable ? "enabled" : "disabled");
    return ESP_OK;
}
