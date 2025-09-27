#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#pragma once

#include "esp_err.h"
#include "usb/uac_host.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_out_init(void);
esp_err_t usb_out_start(void);
esp_err_t usb_out_stop(void);
esp_err_t usb_out_deinit(void);
uac_host_device_handle_t usb_out_get_device_handle(void);
bool usb_out_is_connected(void);

// Audio write functions
esp_err_t usb_out_write(const uint8_t *data, size_t size, TickType_t timeout);
esp_err_t usb_out_stop_playback(void);

// Volume control functions
esp_err_t usb_out_set_volume(float volume);
esp_err_t usb_out_get_volume(float *volume);

// Sleep mode support functions
esp_err_t usb_out_prepare_for_sleep(void);
esp_err_t usb_out_restore_after_wake(void);

// Error handling and recovery functions
esp_err_t usb_out_check_enumeration_timeout(void);

#ifdef __cplusplus
}
#endif