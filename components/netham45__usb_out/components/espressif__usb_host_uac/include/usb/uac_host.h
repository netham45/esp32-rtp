/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <wchar.h>
#include <stdint.h>
#include "esp_err.h"
#include "uac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB UAC HOST string descriptor maximal length
 *
 * The maximum possible number of characters in an embedded string is device specific.
 * For USB devices, the maximum string length is 126 wide characters (not including the terminating NULL character).
 * This is a length, which is available to upper level application during getting information
 * of UAC Device with 'uac_host_get_device_info' call.
 *
 * To decrease memory usage 32 wide characters (64 bytes per every string) is used.
*/
#define UAC_STR_DESC_MAX_LENGTH              (32)

/**
 * @brief Flags to control stream work flow
 *
 * FLAG_STREAM_SUSPEND_AFTER_START: do not start stream transfer during start, only claim interface and prepare memory
 * @note User should call uac_host_device_resume to start stream transfer when needed
*/
#define FLAG_STREAM_SUSPEND_AFTER_START      (1 << 0)

typedef struct uac_interface *uac_host_device_handle_t;    /*!< Logic Device Handle. Handle to a particular UAC interface */

// ------------------------ USB UAC Host events --------------------------------
/**
 * @brief USB UAC HOST Driver event id
*/
typedef enum {
    UAC_HOST_DRIVER_EVENT_RX_CONNECTED = 0x00,           /*!< UAC RX Device has been found in connected USB device */
    UAC_HOST_DRIVER_EVENT_TX_CONNECTED,                  /*!< UAC TX Device has been found in connected USB device */
} uac_host_driver_event_t;

/**
 * @brief USB UAC Device (Interface) event id
*/
typedef enum {
    UAC_HOST_DEVICE_EVENT_RX_DONE = 0x00,                /*!< RX Done: the receive buffer data size exceeds the threshold */
    UAC_HOST_DEVICE_EVENT_TX_DONE,                       /*!< TX Done: the transmit buffer data size falls below the threshold */
    UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,                /*!< UAC Device transfer error */
    UAC_HOST_DRIVER_EVENT_DISCONNECTED,                  /*!< UAC Device has been disconnected */
} uac_host_device_event_t;

// ------------------------ USB UAC Host events callbacks -----------------------------
/**
 * @brief USB UAC driver event callback.
 *
 * @param[in] addr        USB Address of connected UAC device
 * @param[in] iface_num   UAC Interface Number
 * @param[in] event       UAC driver event
 * @param[in] arg         User argument from UAC driver configuration structure
*/
typedef void (*uac_host_driver_event_cb_t)(uint8_t addr, uint8_t iface_num,
        const uac_host_driver_event_t event, void *arg);

/**
 * @brief USB UAC logic device/interface event callback.
 *
 * @param[in] uac_device_handle     UAC device handle (UAC Interface)
 * @param[in] event                 UAC device event
 * @param[in] arg                   User argument
*/
typedef void (*uac_host_device_event_cb_t)(uac_host_device_handle_t uac_device_handle,
        const uac_host_device_event_t event, void *arg);

/**
 * @brief  USB UAC host class descriptor print callback
 *
 * @param[in] desc  Pointer to the USB configuration descriptor
 * @param[in] class  Class of the UAC device
 * @param[in] subclass  Subclass of the UAC device
 * @param[in] protocol  Protocol of the UAC device
 *
 */
typedef void (*print_class_descriptor_with_context_cb)(const usb_standard_desc_t *desc,
        uint8_t class, uint8_t subclass, uint8_t protocol);

/**
 * @brief Stream type
 *
*/
typedef enum {
    UAC_STREAM_TX = 0,     /*!< usb audio TX (eg. speaker stream) */
    UAC_STREAM_RX,         /*!< usb audio RX (eg. microphone stream) */
    UAC_STREAM_MAX,        /*!< max stream type */
} uac_host_stream_t;

/**
 * @brief USB UAC logic device/interface information
*/
typedef struct {
    uac_host_stream_t type;                             /*!< Stream type */
    uint8_t iface_num;                                  /*!< UAC Interface Number */
    uint8_t iface_alt_num;                              /*!< UAC Interface Alternate Setting Total Number */
    uint8_t addr;                                       /*!< USB Address of connected UAC device */
    uint16_t VID;                                       /*!< Vendor ID */
    uint16_t PID;                                       /*!< Product ID */
    wchar_t iManufacturer[UAC_STR_DESC_MAX_LENGTH];     /*!< Manufacturer string */
    wchar_t iProduct[UAC_STR_DESC_MAX_LENGTH];          /*!< Product string */
    wchar_t iSerialNumber[UAC_STR_DESC_MAX_LENGTH];     /*!< Serial Number string */
} uac_host_dev_info_t;

/**
 * @brief USB UAC Interface alternate information
 *
*/
typedef struct {
    uint8_t format;                                  /*!< audio stream format, currently only support 1 - PCM */
    uint8_t channels;                                /*!< audio stream channels */
    uint8_t bit_resolution;                          /*!< audio stream bit resolution */
    uint8_t sample_freq_type;                        /*!< audio stream sample frequency type, 0 - continuous, 1,2,3 - discrete */
    union {
        uint32_t sample_freq[UAC_FREQ_NUM_MAX];      /*!< audio stream sample frequency, first N discrete sample frequency */
        struct {
            uint32_t sample_freq_lower;              /*!< audio stream sample frequency lower */
            uint32_t sample_freq_upper;              /*!< audio stream sample frequency upper */
        };
    };
} uac_host_dev_alt_param_t;

/**
 * @brief UAC driver configuration structure.
*/
typedef struct {
    bool create_background_task;            /*!< When set to true, background task handling USB events is created.
                                             Otherwise user has to periodically call uac_host_handle_events function */
    size_t task_priority;                   /*!< Task priority of created background task */
    size_t stack_size;                      /*!< Stack size of created background task */
    BaseType_t core_id;                     /*!< Select core on which background task will run or tskNO_AFFINITY  */
    uac_host_driver_event_cb_t callback;    /*!< Callback invoked when UAC driver event occurs. Must not be NULL. */
    void *callback_arg;                     /*!< User provided argument passed to callback */
} uac_host_driver_config_t;

/**
 * @brief UAC logic device/interface configuration structure
 *
*/
typedef struct {
    uint8_t addr;                                       /*!< USB Address of connected physical device */
    uint8_t iface_num;                                  /*!< UAC Interface Number */
    uint32_t buffer_size;                               /*!< Audio buffer size */
    uint32_t buffer_threshold;                          /*!< Audio buffer threshold */
    uac_host_device_event_cb_t callback;                /*!< Callback invoked when UAC device event occurs */
    void *callback_arg;                                 /*!< User provided argument passed to callback */
} uac_host_device_config_t;

/**
 * @brief UAC stream configuration structure
 *
*/
typedef struct {
    uint8_t channels;                                    /*!< Audio channel number */
    uint8_t bit_resolution;                              /*!< Audio bit resolution */
    uint32_t sample_freq;                                /*!< Audio sample resolution */
    uint16_t flags;                                      /*!< Control flags */
} uac_host_stream_config_t;

/**
 * @brief UAC Interface alternate setting parameters
 */
typedef struct uac_interface_alt {
    // variable only change by app operation, protected by mutex
    uint32_t cur_sampling_freq;                /*!< current sampling frequency */
    // constant values after interface opening
    uint8_t alt_idx;                           /*!< audio stream alternate setting number */
    uint8_t ep_addr;                           /*!< audio stream endpoint number */
    uint16_t ep_mps;                           /*!< audio stream endpoint max size */
    uint8_t ep_attr;                           /*!< audio stream endpoint attributes */
    uint8_t interval;                          /*!< audio stream endpoint interval */
    uint8_t connected_terminal;                /*!< connected terminal ID */
    uint8_t feature_unit;                      /*!< connected feature unit ID */
    uint8_t vol_ch_map;                        /*!< volume channel map */
    uint8_t mute_ch_map;                       /*!< mute channel map */
    bool freq_ctrl_supported;                  /*!< sampling frequency control supported */
    uac_host_dev_alt_param_t dev_alt_param;    /*!< audio stream alternate setting parameters */
} uac_iface_alt_t;



// ----------------------------- Public ---------------------------------------
/**
 * @brief Install USB Host UAC Class driver
 *
 * @note This function must be called after usb_host_install
 *
 * @param[in] config UAC driver configuration structure
 * @return esp_err_r
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_STATE if UAC driver is already installed
 *  - ESP_ERR_INVALID_ARG if the configuration is invalid
 *  - ESP_ERR_NO_MEM if memory allocation failed
 *
*/
esp_err_t uac_host_install(const uac_host_driver_config_t *config);

/**
 * @brief Uninstall USB Host UAC Class driver
 *
 * @note This function can only be called after all open devices have been closed
 * @return esp_err_t
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_STATE if UAC driver is not installed or device is still opened
 */
esp_err_t uac_host_uninstall(void);

/**
 * @brief Open a UAC logic device/interface
 *
 * @param[in] config            Pointer to UAC device configuration structure
 * @param[out] uac_dev_handle   Pointer to UAC device handle
 * @return esp_err_t
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_STATE if UAC driver is not installed
 *  - ESP_ERR_INVALID_ARG if the configuration is invalid
 *  - ESP_ERR_NO_MEM if memory allocation failed
 *  - ESP_ERR_NOT_SUPPORTED if the UAC version is not supported
 */
esp_err_t uac_host_device_open(const uac_host_device_config_t *config, uac_host_device_handle_t *uac_dev_handle);

/**
 * @brief Close a UAC logic device/interface
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @return esp_err_t
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_STATE if UAC driver is not installed
 *  - ESP_ERR_INVALID_ARG if the device handle is invalid
 */
esp_err_t uac_host_device_close(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Get UAC device information
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @param[out] uac_dev_info   Pointer to UAC device information structure
 * @return esp_err_t
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_STATE if UAC device is not opened
 *  - ESP_ERR_INVALID_ARG if the device handle is invalid
 */
esp_err_t uac_host_get_device_info(uac_host_device_handle_t uac_dev_handle, uac_host_dev_info_t *uac_dev_info);

/**
 * @brief Get UAC device alt setting parameters by interface alternate index
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @param[in] iface_alt       Interface alt setting number
 * @param[out] uac_alt_param  Pointer to UAC device alt setting parameters
 * @return esp_err_t
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_STATE if UAC device is not opened
 *  - ESP_ERR_INVALID_ARG if the device handle or alt setting number is invalid
 */
esp_err_t uac_host_get_device_alt_param(uac_host_device_handle_t uac_dev_handle, uint8_t iface_alt, uac_host_dev_alt_param_t *uac_alt_param);

/**
 * @brief Print the UAC device information and alternate parameters
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if UAC device is not opened
 */
esp_err_t uac_host_printf_device_param(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief UAC Host USB event handler
 *
 * If UAC Host install was made with create_background_task=false configuration,
 * application needs to handle USB Host events itself.
 * Do not used if UAC host install was made with create_background_task=true configuration
 *
 * @param[in]  timeout  Timeout in ticks. For milliseconds, please use 'pdMS_TO_TICKS()' macros
 * @return esp_err_t
 *  - ESP_OK on success (keep calling this function)
 *  - ESP_FAIL if the event handling is finished (stop calling this function)
 */
esp_err_t uac_host_handle_events(uint32_t timeout);

// ------------------------ USB UAC Host driver API ----------------------------
/**
 * @brief Start a UAC stream with specific stream configuration (channels, bit resolution, sample frequency)
 *
 * @note set flags FLAG_STREAM_SUSPEND_AFTER_START to suspend stream after start
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @param[in] stream_config   Pointer to UAC stream configuration structure
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if the device handle or stream configuration is invalid
 * - ESP_ERR_NOT_FOUND if the stream configuration is not supported
 * - ESP_ERR_INVALID_STATE if the device is not in the right state
 * - ESP_ERR_NO_MEM if memory allocation failed
 * - ESP_ERR_TIMEOUT if the control transfer timeout
 */
esp_err_t uac_host_device_start(uac_host_device_handle_t uac_dev_handle, const uac_host_stream_config_t *stream_config);

/**
 * @brief Suspend a UAC stream
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if the device handle is invalid
 * - ESP_ERR_INVALID_STATE if the device is not in the right state
 */
esp_err_t uac_host_device_suspend(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Resume a UAC stream with same stream configuration
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if the device handle is invalid
 * - ESP_ERR_INVALID_STATE if the device is not in the right state
 */
esp_err_t uac_host_device_resume(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Stop a UAC stream, stream resources will be released
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if the device handle is invalid
 * - ESP_ERR_INVALID_STATE if the device is not in the right state
 */
esp_err_t uac_host_device_stop(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Read data from UAC stream buffer, only available after stream started
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @param[out] data           Pointer to the buffer to store the data
 * @param[in] size            Number of bytes to read
 * @param[out] bytes_read     Pointer to the number of bytes read
 * @param[in] timeout         Timeout in ticks. For milliseconds, please use 'pdMS_TO_TICKS()' macros
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if the device handle or data is invalid
 * - ESP_ERR_INVALID_STATE if the device is not in the right state
 */
esp_err_t uac_host_device_read(uac_host_device_handle_t uac_dev_handle, uint8_t *data, uint32_t size,
                               uint32_t *bytes_read, uint32_t timeout);

/**
 * @brief Write data to UAC stream buffer, only can be called after stream started
 *
 * @note The data will be sent to internal ringbuffer before function return,
 * the actual data transfer is scheduled by the background task.
 *
 * @param[in] uac_dev_handle  UAC device handle
 * @param[in] data            Pointer to the data buffer
 * @param[in] size            Number of bytes to write
 * @param[in] timeout         Timeout in ticks. For milliseconds, please use 'pdMS_TO_TICKS()' macros
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if the device handle or data is invalid
 * - ESP_ERR_INVALID_STATE if the device is not in the right state
 * - ESP_FAIL if write failed or timeout
*/
esp_err_t uac_host_device_write(uac_host_device_handle_t uac_dev_handle, uint8_t *data, uint32_t size,
                                uint32_t timeout);

/**
 * @brief Mute or un-mute the UAC device
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] mute        True to mute, false to unmute
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if the device is not ready or active
 * - ESP_ERR_INVALID_ARG if the device handle is invalid
 * - ESP_ERR_NOT_SUPPORTED if the device does not support mute control
 * - ESP_ERR_TIMEOUT if the control timed out
 */
esp_err_t uac_host_device_set_mute(uac_host_device_handle_t uac_dev_handle, bool mute);

/**
 * @brief Set the volume of the UAC device
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] volume      Volume to set, 0-100
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if the device is not ready or active
 * - ESP_ERR_INVALID_ARG if the device handle is invalid or volume is out of range
 * - ESP_ERR_NOT_SUPPORTED if the device does not support volume control
 * - ESP_ERR_TIMEOUT if the control timed out
 */
esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume);

/**
 * @brief Set the volume of the UAC device in dB
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] volume_db   Volume to set, db
 * @return esp_err_t
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if the device is not ready or active
 * - ESP_ERR_INVALID_ARG if the device handle is invalid
 * - ESP_ERR_NOT_SUPPORTED if the device does not support volume control
 * - ESP_ERR_TIMEOUT if the control timed out
 */
esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t uac_dev_handle, uint32_t volume_db);

#ifdef __cplusplus
}
#endif //__cplusplus
