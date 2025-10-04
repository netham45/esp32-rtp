/**
 * @file usb_switch.h
 * @brief USB switch control for TS3USB30ERSWR
 * 
 * This module controls the TS3USB30ERSWR USB 2.0 switch via GPIO pins.
 * The switch is controlled using:
 * - SEL pin (GPIO 16): Selects between port 1 (low) and port 2 (high)
 * - OE pin (GPIO 17): Output enable (active low)
 */

#ifndef USB_SWITCH_H
#define USB_SWITCH_H

#include "esp_err.h"
#include <stdbool.h>

// GPIO Pin definitions for TS3USB30ERSWR
#define USB_SWITCH_SEL_PIN  16  // Selection pin: 0 = Port 1, 1 = Port 2
#define USB_SWITCH_OE_PIN   17  // Output Enable pin: 0 = Enabled, 1 = Disabled

// USB switch port selection
typedef enum {
    USB_SWITCH_PORT_1 = 0,  // Select port 1 (SEL = low)
    USB_SWITCH_PORT_2 = 1   // Select port 2 (SEL = high)
} usb_switch_port_t;

/**
 * @brief Initialize the USB switch
 * 
 * Configures GPIO pins for controlling the TS3USB30ERSWR switch.
 * By default, enables the switch and selects port 1.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t usb_switch_init(void);

/**
 * @brief Set the USB switch mode
 * 
 * @param port Port to select (USB_SWITCH_PORT_1 or USB_SWITCH_PORT_2)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t usb_switch_set_port(usb_switch_port_t port);

/**
 * @brief Enable or disable the USB switch output
 * 
 * @param enable true to enable output, false to disable
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t usb_switch_set_enable(bool enable);

/**
 * @brief Get the current port selection
 * 
 * @return Current port selection (USB_SWITCH_PORT_1 or USB_SWITCH_PORT_2)
 */
usb_switch_port_t usb_switch_get_port(void);

/**
 * @brief Check if the USB switch output is enabled
 * 
 * @return true if enabled, false if disabled
 */
bool usb_switch_is_enabled(void);

#endif // USB_SWITCH_H