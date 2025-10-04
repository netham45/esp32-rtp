/**
 * @file usb_switch.c
 * @brief USB switch control implementation for TS3USB30ERSWR
 */

#include "TS3USB30ERSWR.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "USB_SWITCH";

// Track current state
static usb_switch_port_t current_port = USB_SWITCH_PORT_1;
static bool switch_enabled = false;

esp_err_t usb_switch_init(void)
{
    ESP_LOGI(TAG, "Initializing TS3USB30ERSWR USB switch");
    
    // Configure SEL pin (GPIO 16)
    gpio_config_t sel_conf = {
        .pin_bit_mask = (1ULL << USB_SWITCH_SEL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t err = gpio_config(&sel_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SEL pin (GPIO %d): %s", 
                 USB_SWITCH_SEL_PIN, esp_err_to_name(err));
        return err;
    }
    
    // Configure OE pin (GPIO 17)
    gpio_config_t oe_conf = {
        .pin_bit_mask = (1ULL << USB_SWITCH_OE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    err = gpio_config(&oe_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure OE pin (GPIO %d): %s", 
                 USB_SWITCH_OE_PIN, esp_err_to_name(err));
        return err;
    }
    
    // Set default state: Port 1 selected, switch enabled
    err = usb_switch_set_port(USB_SWITCH_PORT_1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial port: %s", esp_err_to_name(err));
        return err;
    }
    
    err = usb_switch_set_enable(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable switch: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "USB switch initialized (Port 1 selected, output enabled)");
    return ESP_OK;
}

esp_err_t usb_switch_set_port(usb_switch_port_t port)
{
    esp_err_t err = gpio_set_level(USB_SWITCH_SEL_PIN, port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SEL pin to %d: %s", port, esp_err_to_name(err));
        return err;
    }
    
    current_port = port;
    ESP_LOGI(TAG, "USB switch set to port %d", port == USB_SWITCH_PORT_1 ? 1 : 2);
    return ESP_OK;
}

esp_err_t usb_switch_set_enable(bool enable)
{
    // OE is active low, so invert the logic
    int level = enable ? 0 : 1;
    
    esp_err_t err = gpio_set_level(USB_SWITCH_OE_PIN, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set OE pin to %d: %s", level, esp_err_to_name(err));
        return err;
    }
    
    switch_enabled = enable;
    ESP_LOGI(TAG, "USB switch output %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

usb_switch_port_t usb_switch_get_port(void)
{
    return current_port;
}

bool usb_switch_is_enabled(void)
{
    return switch_enabled;
}