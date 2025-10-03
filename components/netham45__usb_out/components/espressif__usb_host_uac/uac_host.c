/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <assert.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "usb/usb_types_ch9.h"
#include "uac_quirks.h"

// UAC spinlock
static portMUX_TYPE uac_lock = portMUX_INITIALIZER_UNLOCKED;
#define UAC_ENTER_CRITICAL()    portENTER_CRITICAL(&uac_lock)
#define UAC_EXIT_CRITICAL()     portEXIT_CRITICAL(&uac_lock)

// UAC verification macros
#define UAC_GOTO_ON_FALSE_CRITICAL(exp, err)    \
    do {                                        \
        if(!(exp)) {                            \
            UAC_EXIT_CRITICAL();                \
            ret = err;                          \
            goto fail;                          \
        }                                       \
    } while(0)

#define UAC_RETURN_ON_FALSE_CRITICAL(exp, err)  \
    do {                                        \
        if(!(exp)) {                            \
            UAC_EXIT_CRITICAL();                \
            return err;                         \
        }                                       \
    } while(0)

#define UAC_GOTO_ON_ERROR(exp, msg) ESP_GOTO_ON_ERROR(exp, fail, TAG, msg)

#define UAC_GOTO_ON_FALSE(exp, err, msg) ESP_GOTO_ON_FALSE((exp), err, fail, TAG, msg)

#define UAC_RETURN_ON_ERROR(exp, msg) ESP_RETURN_ON_ERROR((exp), TAG, msg)

#define UAC_RETURN_ON_FALSE(exp, err, msg) ESP_RETURN_ON_FALSE((exp), (err), TAG, msg)

#define UAC_RETURN_ON_INVALID_ARG(exp) ESP_RETURN_ON_FALSE((exp) != NULL, ESP_ERR_INVALID_ARG, TAG, "Argument error")

// USB Descriptor parsing helping macros
#define GET_NEXT_INTERFACE_DESC(p, max_len, offs)                                                \
    ((const usb_intf_desc_t *)usb_parse_next_descriptor_of_type((const usb_standard_desc_t *)p,  \
                                                                max_len,                         \
                                                                USB_B_DESCRIPTOR_TYPE_INTERFACE, \
                                                                &(offs)))
#define GET_NEXT_UAC_CS_DESC(p, max_len, offs)                                                      \
    ((const uac_desc_header_t *)usb_parse_next_descriptor_of_type((const usb_standard_desc_t *)p, \
                                                                max_len,                         \
                                                                UAC_CS_INTERFACE,   \
                                                                &(offs)))
#define GET_NEXT_DESC(p, max_len, offs)                                                            \
    ((const usb_standard_desc_t *)usb_parse_next_descriptor((const usb_standard_desc_t *)p,       \
                                                            max_len,                               \
                                                            &(offs)))

static const char *TAG = "uac-host";

#define DEFAULT_CTRL_XFER_TIMEOUT_MS        (5000)
#define DEFAULT_ISOC_XFER_TIMEOUT_MS        (100)
#define UAC_SPK_VOLUME_MAX                  (0xfff0)
#define UAC_SPK_VOLUME_MIN                  (0xe3a0)
#define UAC_SPK_VOLUME_STEP                 ((UAC_SPK_VOLUME_MAX - UAC_SPK_VOLUME_MIN)/100)
#define INTERFACE_FLAGS_OFFSET              (16)
#define FLAG_INTERFACE_WAIT_USER_DELETE     (1 << INTERFACE_FLAGS_OFFSET)
#define UAC_EP_DIR_IN                       (0x80)

/**
 * @brief UAC Device structure.
 *
 */
typedef struct uac_host_device {
    // dynamic values after device opening, should be protected by critical section
    STAILQ_ENTRY(uac_host_device) tailq_entry;      /*!< UAC device queue */
    uint8_t opened_cnt;                             /*!< Device opened counter */
    // constant values after device opening
    usb_device_handle_t dev_hdl;                    /*!< USB device handle */
    uint8_t addr;                                   /*!< USB device address */
    SemaphoreHandle_t device_busy;                  /*!< UAC device main mutex */
    SemaphoreHandle_t ctrl_xfer_done;               /*!< Control transfer semaphore */
    usb_transfer_t *ctrl_xfer;                      /*!< Pointer to control transfer buffer */
    uint8_t ctrl_iface_num;                         /*!< Control interface number */
    uint8_t *cs_ac_desc;                            /*!< Class-Specific Audio Control Interface descriptor */
} uac_device_t;

/**
 * @brief UAC Interface state
*/
typedef enum {
    UAC_INTERFACE_STATE_NOT_INITIALIZED = 0x00,     /*!< UAC Interface not initialized */
    UAC_INTERFACE_STATE_IDLE,                       /*!< UAC Interface has been opened but not started */
    UAC_INTERFACE_STATE_READY,                      /*!< UAC Interface has started but stream is suspended */
    UAC_INTERFACE_STATE_ACTIVE,                     /*!< UAC Interface is streaming */
    UAC_INTERFACE_STATE_MAX
} uac_iface_state_t;

/**
 * @brief UAC Interface structure in device to interact with. After UAC device opening keeps the interface configuration
 *
 */
typedef struct uac_interface {
    // dynamic values after interface opening, should be protected by critical section
    STAILQ_ENTRY(uac_interface) tailq_entry;
    usb_transfer_t **xfer_list;                /*!< Pointer to transfer list */
    usb_transfer_t **free_xfer_list;           /*!< Pointer to free transfer list */
    // variable only change by app operation, protected by mutex
    SemaphoreHandle_t state_mutex;             /*!< UAC device state mutex */
    uac_iface_state_t state;                   /*!< Interface state */
    uint32_t flags;                            /*!< Interface flags */
    uint8_t cur_alt;                           /*!< Current alternate setting (-1) */
    // constant parameters after interface opening
    uac_device_t *parent;                      /*!< Parent USB UAC device */
    uint8_t xfer_num;                          /*!< Number of transfers */
    uint8_t packet_num;                        /*!< packets per transfer */
    uint32_t packet_size;                      /*!< size of each packet */
    uac_host_device_event_cb_t user_cb;        /*!< Interface application callback */
    void *user_cb_arg;                         /*!< Interface application callback arg */
    RingbufHandle_t ringbuf;                   /*!< Ring buffer for audio data */
    uint32_t ringbuf_size;                     /*!< Ring buffer size */
    uint32_t ringbuf_threshold;                /*!< Ring buffer threshold */
    uac_host_dev_info_t dev_info;              /*!< USB device parameters */
    uac_iface_alt_t *iface_alt;                /*!< audio stream alternate setting */
} uac_iface_t;

/**
 * @brief UAC driver default context
 *
 * This context is created during UAC Host install.
 */
typedef struct {
    // dynamic values after UAC Host initialization, should be protected by critical section
    STAILQ_HEAD(devices, uac_host_device) uac_devices_tailq;    /*!< STAILQ of UAC interfaces */
    STAILQ_HEAD(interfaces, uac_interface) uac_ifaces_tailq;    /*!< STAILQ of UAC interfaces */
    volatile bool end_client_event_handling;                    /*!< Client event handling flag */
    // constant values after UAC Host initialization
    bool event_handling_started;                                /*!< Events handler started flag */
    usb_host_client_handle_t client_handle;                     /*!< Client task handle */
    uac_host_driver_event_cb_t user_cb;                         /*!< User application callback */
    void *user_arg;                                             /*!< User application callback args */
    SemaphoreHandle_t all_events_handled;                       /*!< Events handler semaphore */
} uac_driver_t;

/**
 * @brief UAC class specific request
*/
typedef struct uac_cs_request {
    uint8_t bmRequestType;          /*!< bmRequestType */
    uint8_t bRequest;               /*!< bRequest  */
    uint16_t wValue;                /*!< wValue: Report Type and Report ID */
    uint16_t wIndex;                /*!< wIndex: Interface */
    uint16_t wLength;               /*!< wLength: Report Length */
    uint8_t *data;                  /*!< Pointer to data */
} uac_cs_request_t;

static uac_driver_t *s_uac_driver;                              /*!< Internal pointer to UAC driver */

// ----------------------- Private Prototypes ----------------------------------

static esp_err_t _uac_host_device_add(uint8_t addr, usb_device_handle_t dev_hdl, const usb_config_desc_t *config_desc, uac_device_t **uac_device_handle);
static esp_err_t _uac_host_device_delete(uac_device_t *uac_device);
static esp_err_t uac_cs_request_set(uac_device_t *uac_device, const uac_cs_request_t *req);
static esp_err_t uac_cs_request_set_ep_frequency(uac_iface_t *iface, uint8_t ep_addr, uint32_t freq);

// --------------------------- Buffer Management --------------------------------
static size_t _ring_buffer_get_len(RingbufHandle_t ringbuf_hdl)
{
    assert(ringbuf_hdl);
    size_t size = 0;
    vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &size);
    return size;
}

static void _ring_buffer_flush(RingbufHandle_t ringbuf_hdl)
{
    assert(ringbuf_hdl);
    size_t read_bytes = 0;
    size_t uxItemsWaiting = 0;
    vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &uxItemsWaiting);
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes, 0, uxItemsWaiting);

    if (buf_rcv) {
        vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
    }

    if (uxItemsWaiting > read_bytes) {
        // read the second time to flush all data
        vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &uxItemsWaiting);
        buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes, 0, uxItemsWaiting);
        if (buf_rcv) {
            vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
        }
    }
}

static esp_err_t _ring_buffer_push(RingbufHandle_t ringbuf_hdl, uint8_t *buf, size_t write_bytes, TickType_t xTicksToWait)
{
    assert(ringbuf_hdl && buf);
    int res = xRingbufferSend(ringbuf_hdl, buf, write_bytes, xTicksToWait);

    if (res != pdTRUE) {
        ESP_LOGD(TAG, "buffer is too small, push failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t _ring_buffer_pop(RingbufHandle_t ringbuf_hdl, uint8_t *buf, size_t req_bytes, size_t *read_bytes, TickType_t ticks_to_wait)
{
    assert(ringbuf_hdl && buf && read_bytes);
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, read_bytes, ticks_to_wait, req_bytes);

    if (!buf_rcv) {
        return ESP_FAIL;
    }

    memcpy(buf, buf_rcv, *read_bytes);
    vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));

    size_t read_bytes2 = 0;
    if (*read_bytes < req_bytes) {
        buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes2, 0, req_bytes - *read_bytes);
        if (buf_rcv) {
            memcpy(buf + *read_bytes, buf_rcv, read_bytes2);
            *read_bytes += read_bytes2;
            vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
        }
    }

    return ESP_OK;
}

/**
 * @brief UAC Host driver event handler internal task
 *
 */
static void event_handler_task(void *arg)
{
    ESP_LOGD(TAG, "USB UAC handling start");
    while (uac_host_handle_events(portMAX_DELAY) == ESP_OK) {
    }
    ESP_LOGD(TAG, "USB UAC handling stop");
    vTaskDelete(NULL);
}

/**
 * @brief Return UAC device in devices list by USB device handle
 *
 * @param[in] usb_device_handle_t   USB device handle
 * @return uac_device_t Pointer to device, NULL if device not present
 */
static uac_device_t *get_uac_device_by_handle(usb_device_handle_t usb_handle)
{
    uac_device_t *device = NULL;

    UAC_ENTER_CRITICAL();
    STAILQ_FOREACH(device, &s_uac_driver->uac_devices_tailq, tailq_entry) {
        if (usb_handle == device->dev_hdl) {
            UAC_EXIT_CRITICAL();
            return device;
        }
    }
    UAC_EXIT_CRITICAL();
    return NULL;
}

/**
 * @brief Return UAC Device from usb address
 * @param[in] addr  USB device address
 * @return uac_device_t Pointer to UAC Device
 */
static uac_device_t *get_uac_device_by_addr(uint8_t addr)
{
    uac_device_t *device = NULL;

    UAC_ENTER_CRITICAL();
    STAILQ_FOREACH(device, &s_uac_driver->uac_devices_tailq, tailq_entry) {
        if (addr == device->addr) {
            UAC_EXIT_CRITICAL();
            return device;
        }
    }
    UAC_EXIT_CRITICAL();
    return NULL;
}


/**
 * @brief Get UAC Interface pointer by Interface number
 *
 * @param[in] addr         USB device address
 * @param[in] iface_num    Interface number
 * @return uac_iface_t     Pointer to UAC Interface configuration structure
 */
static uac_iface_t *get_interface_by_addr(uint8_t addr, uint8_t iface_num)
{
    uac_iface_t *interface = NULL;

    UAC_ENTER_CRITICAL();
    STAILQ_FOREACH(interface, &s_uac_driver->uac_ifaces_tailq, tailq_entry) {
        if (addr == interface->dev_info.addr && iface_num == interface->dev_info.iface_num) {
            UAC_EXIT_CRITICAL();
            return interface;
        }
    }

    UAC_EXIT_CRITICAL();
    return NULL;
}

/**
 * @brief Verify presence of Interface in the RAM list
 *
 * @param[in] iface         Pointer to an Interface structure
 * @return true             Interface is in the list
 * @return false            Interface is not in the list
 */
static inline bool is_interface_in_list(uac_iface_t *iface)
{
    uac_iface_t *interface = NULL;

    UAC_ENTER_CRITICAL();
    STAILQ_FOREACH(interface, &s_uac_driver->uac_ifaces_tailq, tailq_entry) {
        if (iface == interface) {
            UAC_EXIT_CRITICAL();
            return true;
        }
    }

    UAC_EXIT_CRITICAL();
    return false;
}

/**
 * @brief Get UAC Interface pointer by external UAC Device handle with verification in RAM list
 *
 * @param[in] uac_dev_handle UAC Device handle
 * @return uac_iface_t       Pointer to an Interface structure
 */
static uac_iface_t *get_iface_by_handle(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *uac_iface = (uac_iface_t *) uac_dev_handle;

    if (!is_interface_in_list(uac_iface)) {
        ESP_LOGE(TAG, "UAC interface handle not found");
        return NULL;
    }

    return uac_iface;
}

/**
 * @brief Check UAC interface descriptor present
 *
 * @param[in] config_desc  Pointer to Configuration Descriptor
 * @return esp_err_t
 */
static bool uac_interface_present(const usb_config_desc_t *config_desc)
{
    assert(config_desc);
    int offset = 0;
    int total_len = config_desc->wTotalLength;
    const usb_intf_desc_t *iface_desc = GET_NEXT_INTERFACE_DESC(config_desc, total_len, offset);
    while (iface_desc != NULL) {
        if (USB_CLASS_AUDIO == iface_desc->bInterfaceClass) {
            return true;
        }
        iface_desc = GET_NEXT_INTERFACE_DESC(iface_desc, total_len, offset);
    }
    return false;
}

/**
 * @brief UAC Interface user callback function.
 *
 * @param[in] uac_iface   Pointer to an Interface structure
 * @param[in] event       UAC Interface event
 */
static inline void uac_host_user_interface_callback(uac_iface_t *uac_iface, const uac_host_device_event_t event)
{
    assert(uac_iface);
    // user callback should never block
    if (uac_iface->user_cb) {
        uac_iface->user_cb(uac_iface, event, uac_iface->user_cb_arg);
    }
}

/**
 * @brief UAC Device user callback function.
 *
 * @param[in] event_id    UAC Device event
 * @param[in] event       UAC Device event
 */
static inline void uac_host_user_device_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event)
{
    if (s_uac_driver && s_uac_driver->user_cb) {
        s_uac_driver->user_cb(addr, iface_num, event, s_uac_driver->user_arg);
    }
}

static esp_err_t uac_host_string_descriptor_copy(wchar_t *dest, const usb_str_desc_t *src)
{
    if (dest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src != NULL) {
        size_t len = MIN((src->bLength - USB_STANDARD_DESC_SIZE) / 2, UAC_STR_DESC_MAX_LENGTH - 1);
        for (int i = 0; i < len; i++) {
            dest[i] = (wchar_t) src->wData[i];
        }
        // This should be always true, we just check to avoid LoadProhibited exception
        if (dest != NULL) {
            dest[len] = 0;
        }
    } else {
        dest[0] = 0;
    }
    return ESP_OK;
}

/** Lock UAC interface when user tries to change the interface state
 *
 * @param[in] iface         Pointer to UAC interface structure
 * @param[in] timeout_ms    Timeout of trying to take the mutex
 * @return esp_err_t
 */
static inline esp_err_t uac_host_interface_try_lock(uac_iface_t *iface, uint32_t timeout_ms)
{
    return (xSemaphoreTake(iface->state_mutex, pdMS_TO_TICKS(timeout_ms)) ? ESP_OK : ESP_ERR_TIMEOUT);
}

/** Unlock UAC interface when user changes the interface state
 *
 * @param[in] iface         Pointer to UAC interface structure
 * @return esp_err_t
 */
static inline esp_err_t uac_host_interface_unlock(uac_iface_t *iface)
{
    return (xSemaphoreGive(iface->state_mutex) ? ESP_OK : ESP_FAIL);
}

static uint8_t _uac_next_linked_uint_id(const uint8_t *desc, uint8_t unit_id, uint8_t **feat_desc)
{
    *feat_desc = NULL;
    if (!desc) {
        return 0;
    }
    uac_ac_header_desc_t *header_desc = (uac_ac_header_desc_t *)desc;
    const size_t total_length = header_desc->wTotalLength;
    int uac_desc_offset = 0;
    const uac_desc_header_t *uac_cs_desc = (const uac_desc_header_t *)header_desc;
    while (uac_cs_desc) {
        switch (uac_cs_desc->bDescriptorSubtype) {
        case UAC_AC_HEADER:
        case UAC_AC_INPUT_TERMINAL:
            break;
        case UAC_AC_OUTPUT_TERMINAL: {
            const uac_ac_output_terminal_desc_t *output_terminal_desc = (const uac_ac_output_terminal_desc_t *)uac_cs_desc;
            if (output_terminal_desc->bSourceID == unit_id) {
                return output_terminal_desc->bTerminalID;
            }
            break;
        }
        case UAC_AC_FEATURE_UNIT: {
            const uac_ac_feature_unit_desc_t *feature_unit_desc = (const uac_ac_feature_unit_desc_t *)uac_cs_desc;
            if (feature_unit_desc->bSourceID == unit_id) {
                *feat_desc = (uint8_t *)feature_unit_desc;
                return feature_unit_desc->bUnitID;
            }
            break;
        }
        case UAC_AC_SELECTOR_UNIT: {
            const uac_ac_selector_unit_desc_t *selector_unit_desc = (const uac_ac_selector_unit_desc_t *)uac_cs_desc;
            for (int i = 0; i < selector_unit_desc->bNrInPins; i++) {
                if (selector_unit_desc->baSourceID[i] == unit_id) {
                    return selector_unit_desc->bUnitID;
                }
            }
            break;
        }
        case UAC_AC_MIXER_UNIT: {
            const uac_ac_mixer_unit_desc_t *mixer_unit_desc = (const uac_ac_mixer_unit_desc_t *)uac_cs_desc;
            for (int i = 0; i < mixer_unit_desc->bNrInPins; i++) {
                if (mixer_unit_desc->baSourceID[i] == unit_id) {
                    return mixer_unit_desc->bUnitID;
                }
            }
            break;
        }
        default:
            ESP_LOGD(TAG, "UAC Unknown Descriptor Subtype %d", uac_cs_desc->bDescriptorSubtype);
            break;
        }
        uac_cs_desc = (uac_desc_header_t *)GET_NEXT_DESC(uac_cs_desc, total_length, uac_desc_offset);
    }
    return 0;
}

static uint8_t _uac_last_linked_uint_id(const uint8_t *desc, uint8_t unit_id, uint8_t **feat_desc)
{
    *feat_desc = NULL;
    if (!desc) {
        return 0;
    }
    uac_ac_header_desc_t *header_desc = (uac_ac_header_desc_t *)desc;
    const size_t total_length = header_desc->wTotalLength;
    int uac_desc_offset = 0;
    const uac_desc_header_t *uac_cs_desc = (const uac_desc_header_t *)header_desc;
    while (uac_cs_desc) {
        switch (uac_cs_desc->bDescriptorSubtype) {
        case UAC_AC_HEADER:
        case UAC_AC_INPUT_TERMINAL:
            break;
        case UAC_AC_OUTPUT_TERMINAL: {
            const uac_ac_output_terminal_desc_t *output_terminal_desc = (const uac_ac_output_terminal_desc_t *)uac_cs_desc;
            if (output_terminal_desc->bTerminalID == unit_id) {
                return output_terminal_desc->bSourceID;
            }
            break;
        }
        case UAC_AC_FEATURE_UNIT: {
            const uac_ac_feature_unit_desc_t *feature_unit_desc = (const uac_ac_feature_unit_desc_t *)uac_cs_desc;
            if (feature_unit_desc->bUnitID == unit_id) {
                *feat_desc = (uint8_t *)feature_unit_desc;
                return feature_unit_desc->bSourceID;
            }
            break;
        }
        case UAC_AC_SELECTOR_UNIT: {
            const uac_ac_selector_unit_desc_t *selector_unit_desc = (const uac_ac_selector_unit_desc_t *)uac_cs_desc;
            if (selector_unit_desc->bUnitID == unit_id) {
                // for basic audio device, the first source should be microphone
                return selector_unit_desc->baSourceID[0];
            }
            break;
        }
        case UAC_AC_MIXER_UNIT: {
            const uac_ac_mixer_unit_desc_t *mixer_unit_desc = (const uac_ac_mixer_unit_desc_t *)uac_cs_desc;
            if (mixer_unit_desc->bUnitID == unit_id) {
                return mixer_unit_desc->baSourceID[0];
            }
            break;
        }
        default:
            ESP_LOGD(TAG, "UAC Unknown Descriptor Subtype %d", uac_cs_desc->bDescriptorSubtype);
            break;
        }
        uac_cs_desc = (uac_desc_header_t *)GET_NEXT_DESC(uac_cs_desc, total_length, uac_desc_offset);
    }
    return 0;
}

static uac_ac_feature_unit_desc_t *_uac_host_device_find_feature_unit(const uint8_t *header_desc, uint8_t terminal_id, bool if_input)
{
    if (!header_desc) {
        return NULL;
    }

    uint8_t unit_id = terminal_id;
    uac_ac_feature_unit_desc_t *feature_unit_desc = NULL;
    if (if_input) {
        while (feature_unit_desc == NULL && unit_id != 0) {
            unit_id = _uac_next_linked_uint_id(header_desc, unit_id, (uint8_t **)&feature_unit_desc);
            ESP_LOGD(TAG, "Input Terminal next linked unit ID %d", unit_id);
        }
    } else {
        while (feature_unit_desc == NULL && unit_id != 0) {
            unit_id = _uac_last_linked_uint_id(header_desc, unit_id, (uint8_t **)&feature_unit_desc);
            ESP_LOGD(TAG, "Output Terminal last linked unit ID %d", unit_id);
        }
    }

    return feature_unit_desc;
}

/**
 * @brief Add a new logical device/interface to the UAC driver
 * @param[in] iface_num     Interface number
 * @param[out] p_uac_iface  Pointer to the UAC interface handle
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_add(uac_device_t *uac_device, uint8_t iface_num, uac_iface_t **p_uac_iface)
{
    esp_err_t ret;
    *p_uac_iface = NULL;
    uac_iface_t *uac_iface = calloc(1, sizeof(uac_iface_t));
    UAC_RETURN_ON_FALSE(uac_iface, ESP_ERR_NO_MEM, "Unable to allocate memory");
    uac_iface->state_mutex = xSemaphoreCreateMutex();
    UAC_GOTO_ON_FALSE(uac_iface->state_mutex, ESP_ERR_NO_MEM, "Unable to create state mutex");
    const usb_config_desc_t *config_desc = NULL;
    const usb_intf_desc_t *iface_desc = NULL;
    const usb_intf_desc_t *iface_alt_desc = NULL;
    const usb_ep_desc_t *ep_desc = NULL;
    const usb_standard_desc_t *cs_desc = NULL;

    usb_host_get_active_config_descriptor(uac_device->dev_hdl, &config_desc);
    UAC_GOTO_ON_FALSE(config_desc, ESP_ERR_INVALID_STATE, "No active configuration descriptor");
    const size_t total_length = config_desc->wTotalLength;
    int iface_alt_offset = 0;
    int iface_alt_idx = 0;

    iface_desc = usb_parse_interface_descriptor(config_desc, iface_num, 0, &iface_alt_offset);
    iface_alt_desc = GET_NEXT_INTERFACE_DESC(iface_desc, total_length, iface_alt_offset);
    // For every alternate setting
    while (iface_alt_desc != NULL) {
        // Check if the alternate setting is for the same interface
        if (iface_alt_desc->bInterfaceNumber != iface_desc->bInterfaceNumber) {
            break;
        }
        ESP_LOGD(TAG, "Found UAC bInterfaceNumber= %d, bAlternateSetting= %d",
                 iface_alt_desc->bInterfaceNumber, iface_alt_desc->bAlternateSetting);
        iface_alt_idx++;
        // Allocate memory for the alternate setting
        uac_iface->iface_alt = realloc(uac_iface->iface_alt, iface_alt_idx * sizeof(uac_iface_alt_t));
        UAC_GOTO_ON_FALSE(uac_iface->iface_alt, ESP_ERR_NO_MEM, "Unable to allocate memory");
        uac_iface_alt_t *iface_alt = &uac_iface->iface_alt[iface_alt_idx - 1];
        memset(iface_alt, 0, sizeof(uac_iface_alt_t));
        iface_alt->alt_idx = iface_alt_desc->bAlternateSetting;
        // Parse each descriptor following the alternate interface descriptor
        int cs_offset = iface_alt_offset;
        cs_desc = GET_NEXT_DESC(iface_alt_desc, total_length, cs_offset);
        bool parse_continue = true;
        while (cs_desc != NULL && parse_continue) {
            switch (cs_desc->bDescriptorType) {
            case UAC_CS_INTERFACE: {
                const uac_desc_header_t *uac_desc = (const uac_desc_header_t *)cs_desc;
                if (uac_desc->bDescriptorSubtype == UAC_AS_GENERAL) {
                    const uac_as_general_desc_t *as_general_desc = (const uac_as_general_desc_t *)uac_desc;
                    iface_alt->dev_alt_param.format = as_general_desc->wFormatTag;
                    iface_alt->connected_terminal = as_general_desc->bTerminalLink;
                } else if (uac_desc->bDescriptorSubtype == UAC_AS_FORMAT_TYPE) {
                    const uac_as_type_I_format_desc_t *as_format_type_desc = (const uac_as_type_I_format_desc_t *)uac_desc;
                    if (as_format_type_desc->bFormatType != UAC_FORMAT_TYPE_I) {
                        ESP_LOGE(TAG, "UAC Format Type %d", as_format_type_desc->bFormatType);
                        UAC_GOTO_ON_FALSE(0, ESP_ERR_NOT_SUPPORTED, "UAC Format Type not supported");
                    }
                    iface_alt->dev_alt_param.channels = as_format_type_desc->bNrChannels;
                    iface_alt->dev_alt_param.bit_resolution = as_format_type_desc->bBitResolution;
                    iface_alt->dev_alt_param.sample_freq_type = as_format_type_desc->bSamFreqType;
                    if (as_format_type_desc->bSamFreqType == 0) {
                        iface_alt->dev_alt_param.sample_freq_lower = (as_format_type_desc->tSamFreq[2] << 16) | (as_format_type_desc->tSamFreq[1] << 8) | as_format_type_desc->tSamFreq[0];
                        iface_alt->dev_alt_param.sample_freq_upper = (as_format_type_desc->tSamFreq[5] << 16) | (as_format_type_desc->tSamFreq[4] << 8) | as_format_type_desc->tSamFreq[3];
                    } else {
                        for (int i = 0; i < as_format_type_desc->bSamFreqType && i < UAC_FREQ_NUM_MAX; i++) {
                            iface_alt->dev_alt_param.sample_freq[i] = (as_format_type_desc->tSamFreq[i * 3 + 2] << 16) | (as_format_type_desc->tSamFreq[i * 3 + 1] << 8) | as_format_type_desc->tSamFreq[i * 3];
                        }
                        if (as_format_type_desc->bSamFreqType > UAC_FREQ_NUM_MAX) {
                            ESP_LOGW(TAG, "UAC Interface %d->%d, Frequency Number %d exceed the maximum %d", iface_desc->bInterfaceNumber, iface_alt->alt_idx, as_format_type_desc->bSamFreqType, UAC_FREQ_NUM_MAX);
                        }
                    }
                    ESP_LOGD(TAG, "UAC AS Format Type %d, Bit Resolution %d, Sample Freq Type %d", as_format_type_desc->bFormatType, as_format_type_desc->bBitResolution, as_format_type_desc->bSamFreqType);
                } else {
                    ESP_LOGW(TAG, "UAC CS bDescriptorSubtype=%d not supported", uac_desc->bDescriptorSubtype);
                }
                break;
            }
            case USB_B_DESCRIPTOR_TYPE_ENDPOINT: {
                ep_desc = (const usb_ep_desc_t *)cs_desc;
                iface_alt->ep_addr = ep_desc->bEndpointAddress;
                iface_alt->ep_mps = ep_desc->wMaxPacketSize;
                iface_alt->ep_attr = ep_desc->bmAttributes;
                iface_alt->interval = ep_desc->bInterval;
                uac_iface->dev_info.type = (ep_desc->bEndpointAddress & UAC_EP_DIR_IN) ? UAC_STREAM_RX : UAC_STREAM_TX;
                uac_ac_feature_unit_desc_t *feature_unit_desc = _uac_host_device_find_feature_unit((uint8_t *)uac_device->cs_ac_desc,
                        iface_alt->connected_terminal, !(ep_desc->bEndpointAddress & UAC_EP_DIR_IN));
                if (feature_unit_desc) {
                    iface_alt->feature_unit = feature_unit_desc->bUnitID;
                    uint8_t ch_num = 0;
                    for (size_t i = 0; i < (feature_unit_desc->bLength - 7) / feature_unit_desc->bControlSize; i++) {
                        if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_VOLUME) {
                            iface_alt->vol_ch_map |= (1 << ch_num);
                        }
                        if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_MUTE) {
                            iface_alt->mute_ch_map |= (1 << ch_num);
                        }
                        ch_num++;
                    }
                    ESP_LOGD(TAG, "UAC %s Feature Unit ID %d, Volume Ch Map %02X, Mute Ch Map %02X", uac_iface->dev_info.type == UAC_STREAM_RX ? "RX" : "TX",
                             feature_unit_desc->bUnitID, iface_alt->vol_ch_map, iface_alt->mute_ch_map);
                }
                ESP_LOGD(TAG, "UAC Endpoint 0x%02X, Max Packet Size %d, Attributes 0x%02X, Interval %d", ep_desc->bEndpointAddress, ep_desc->wMaxPacketSize, ep_desc->bmAttributes, ep_desc->bInterval);
                break;
            }
            case UAC_CS_ENDPOINT: {
                // we has got enough information to fill the uac_iface_alt_t, so we can break
                const uac_as_cs_ep_desc_t *cs_ep_desc = (const uac_as_cs_ep_desc_t *)cs_desc;
                if (cs_ep_desc->bDescriptorSubtype == UAC_EP_GENERAL) {
                    iface_alt->freq_ctrl_supported = cs_ep_desc->bmAttributes & UAC_SAMPLING_FREQ_CONTROL;
                    parse_continue = false;
                    ESP_LOGD(TAG, "UAC EP General, Attributes 0x%02X", cs_ep_desc->bmAttributes);
                    ESP_LOGD(TAG, "UAC EP Frequency Control %d", iface_alt->freq_ctrl_supported);
                }
                break;
            }
            default:
                break;
            }
            cs_desc = GET_NEXT_DESC(cs_desc, total_length, cs_offset);
        }
        // Get next alternate setting
        iface_alt_desc = GET_NEXT_INTERFACE_DESC(iface_alt_desc, total_length, iface_alt_offset);
    }
    uac_iface->state = UAC_INTERFACE_STATE_NOT_INITIALIZED;
    uac_iface->parent = uac_device;
    uac_iface->dev_info.addr = uac_device->addr;
    uac_iface->dev_info.iface_num = iface_desc->bInterfaceNumber;
    uac_iface->dev_info.iface_alt_num = iface_alt_idx;
    ESP_LOGD(TAG, "UAC Interface %d, found total alternate %d", iface_desc->bInterfaceNumber, iface_alt_idx);
    
    // Check for device quirks
    const usb_device_desc_t *dev_desc;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(uac_device->dev_hdl, &dev_desc));
    
    // Apply quirks to each alternate setting
    if (iface_alt_idx > 0) {
        // For each alternate setting, check if there's a matching quirk
        for (int i = 0; i < iface_alt_idx; i++) {
            // Check for endpoint quirks for this specific alternate setting
            uac_quirk_t quirk;
            
            // Try to find a quirk for this device, interface, and alternate setting
            if (uac_check_endpoint_quirk(dev_desc, iface_desc->bInterfaceNumber, i, &quirk)) {
                // Apply the quirk to this alternate setting
                uac_apply_endpoint_quirk(&uac_iface->iface_alt[i], &quirk, i);
                ESP_LOGI(TAG, "Applied quirk for device VID=0x%04x, PID=0x%04x, interface %d, alt_setting %d",
                         quirk.vid, quirk.pid, quirk.endpoint.iface_num, i);
            }
        }
    }

    // Fill descriptor device information
    const usb_device_desc_t *desc;
    usb_device_info_t dev_info;
    UAC_GOTO_ON_ERROR(usb_host_get_device_descriptor(uac_device->dev_hdl, &desc), "Unable to get device descriptor");
    UAC_GOTO_ON_ERROR(usb_host_device_info(uac_device->dev_hdl, &dev_info), "Unable to get USB device info");
    // VID, PID
    uac_iface->dev_info.VID = desc->idVendor;
    uac_iface->dev_info.PID = desc->idProduct;
    // Strings
    uac_host_string_descriptor_copy(uac_iface->dev_info.iManufacturer, dev_info.str_desc_manufacturer);
    uac_host_string_descriptor_copy(uac_iface->dev_info.iProduct, dev_info.str_desc_product);
    uac_host_string_descriptor_copy(uac_iface->dev_info.iSerialNumber, dev_info.str_desc_serial_num);

    *p_uac_iface = uac_iface;

    UAC_ENTER_CRITICAL();
    STAILQ_INSERT_TAIL(&s_uac_driver->uac_ifaces_tailq, uac_iface, tailq_entry);
    UAC_EXIT_CRITICAL();

    return ESP_OK;

fail:
    if (uac_iface && uac_iface->state_mutex) {
        vSemaphoreDelete(uac_iface->state_mutex);
    }
    free(uac_iface->iface_alt);
    free(uac_iface);
    return ret;
}

/**
 * @brief Remove interface from a list
 *
 *
 * @param[in] uac_iface    UAC interface handle
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_delete(uac_iface_t *uac_iface)
{
    uac_iface->state = UAC_INTERFACE_STATE_NOT_INITIALIZED;
    UAC_ENTER_CRITICAL();
    STAILQ_REMOVE(&s_uac_driver->uac_ifaces_tailq, uac_iface, uac_interface, tailq_entry);
    UAC_EXIT_CRITICAL();
    vSemaphoreDelete(uac_iface->state_mutex);
    free(uac_iface->iface_alt);
    free(uac_iface);
    return ESP_OK;
}

/**
 * @brief Check every interface in the USB device, notify user about connected interfaces/logic devices
 *
 * @param[in] addr         USB device address
 * @param[in] config_desc  Pointer to Configuration Descriptor
 * @return esp_err_t
 * @retval ESP_OK          UAC Interface found
 * @retval ESP_ERR_NOT_FOUND UAC Interface not found
 */
static esp_err_t uac_host_interface_check(uint8_t addr, const usb_config_desc_t *config_desc)
{
    assert(config_desc);
    size_t total_length = config_desc->wTotalLength;
    const usb_intf_desc_t *iface_desc = NULL;
    int iface_offset = 0;
    bool is_uac_interface = false;

    // Get first Interface descriptor
    iface_desc = GET_NEXT_INTERFACE_DESC(config_desc, total_length, iface_offset);
    // Check every uac stream interface
    while (iface_desc != NULL) {
        if (iface_desc->bInterfaceClass == USB_CLASS_AUDIO && iface_desc->bInterfaceSubClass == UAC_SUBCLASS_AUDIOSTREAMING) {
            // notify user about the connected Interfaces
            is_uac_interface = true;
            const usb_intf_desc_t *iface_alt_desc = GET_NEXT_INTERFACE_DESC(iface_desc, total_length, iface_offset);
            int ep_offset = iface_offset;
            const usb_ep_desc_t *ep_desc = usb_parse_endpoint_descriptor_by_index(iface_alt_desc, 0, total_length, &ep_offset);
            if (ep_desc == NULL) {
                ESP_LOGW(TAG, "No endpoint descriptor found");
            } else if (ep_desc->bEndpointAddress & UAC_EP_DIR_IN) {
                // notify user about the connected Interfaces
                uac_host_user_device_callback(addr, iface_desc->bInterfaceNumber, UAC_HOST_DRIVER_EVENT_RX_CONNECTED);
            } else {
                // notify user about the connected Interfaces
                uac_host_user_device_callback(addr, iface_desc->bInterfaceNumber, UAC_HOST_DRIVER_EVENT_TX_CONNECTED);
            }
            // Skip all alternate settings belonging to the current interface
            while (iface_alt_desc != NULL) {
                // Check if the alternate setting is for the same interface
                if (iface_alt_desc->bInterfaceNumber != iface_desc->bInterfaceNumber) {
                    break;
                }
                iface_alt_desc = GET_NEXT_INTERFACE_DESC(iface_alt_desc, total_length, iface_offset);
            }
            iface_desc = iface_alt_desc;
            continue;
        }
        iface_desc = GET_NEXT_INTERFACE_DESC(iface_desc, total_length, iface_offset);
    }

    return is_uac_interface ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/**
 * @brief Handler for USB device connected event
 *
 * @param[in] addr   USB device physical address
 * @return esp_err_t
 */
static esp_err_t _uac_host_device_connected(uint8_t addr)
{
    bool is_uac_device = false;
    usb_device_handle_t dev_hdl;
    const usb_config_desc_t *config_desc = NULL;

    if (usb_host_device_open(s_uac_driver->client_handle, addr, &dev_hdl) == ESP_OK) {
        if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) == ESP_OK) {
            is_uac_device = uac_interface_present(config_desc);
        }
        UAC_RETURN_ON_ERROR(usb_host_device_close(s_uac_driver->client_handle, dev_hdl), "Unable to close USB device");
    }

    // Create UAC interfaces list in RAM, connected to the particular USB dev
    if (is_uac_device) {
#ifdef CONFIG_PRINTF_UAC_CONFIGURATION_DESCRIPTOR
        print_uac_descriptors(config_desc);
#endif
        // Create Interfaces list for a possibility to claim Interface
        UAC_RETURN_ON_ERROR(uac_host_interface_check(addr, config_desc), "uac stream interface not found");
    } else {
        ESP_LOGW(TAG, "USB device with addr(%d) is not UAC device", addr);
    }

    return is_uac_device ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/**
 * @brief USB device was removed we need to shutdown UAC Interface
 *
 * @param[in] uac_dev_handle    Handle of the UAC device to close
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_shutdown(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *uac_iface = get_iface_by_handle(uac_dev_handle);

    UAC_RETURN_ON_INVALID_ARG(uac_iface);

    if (uac_iface->user_cb) {
        // Let user handle the remove process
        // clear the flag FLAG_INTERFACE_WAIT_USER_DELETE;
        uac_iface->flags &= ~FLAG_INTERFACE_WAIT_USER_DELETE;
        uac_host_user_interface_callback(uac_iface, UAC_HOST_DRIVER_EVENT_DISCONNECTED);
    } else {
        // Remove UAC Interface from the list right now
        uac_iface->flags &= ~FLAG_INTERFACE_WAIT_USER_DELETE;
        uac_host_device_close(uac_dev_handle);
    }

    return ESP_OK;
}

/**
 * @brief Handler for USB device disconnected event
 *
 * @param[in] dev_hdl   USB device handle
 * @return esp_err_t
 */
static esp_err_t _uac_host_device_disconnected(usb_device_handle_t dev_hdl)
{
    uac_device_t *uac_device = get_uac_device_by_handle(dev_hdl);
    uac_iface_t *uac_iface = NULL;
    // Device should be in the list
    assert(uac_device);

    UAC_ENTER_CRITICAL();
    while (!STAILQ_EMPTY(&s_uac_driver->uac_ifaces_tailq)) {
        uac_iface = STAILQ_FIRST(&s_uac_driver->uac_ifaces_tailq);
        UAC_EXIT_CRITICAL();
        if (uac_iface->parent && (uac_iface->parent->addr == uac_device->addr)) {
            uac_iface->flags |= FLAG_INTERFACE_WAIT_USER_DELETE;
            UAC_RETURN_ON_ERROR(uac_host_device_close(uac_iface), "Unable to close device");
            UAC_RETURN_ON_ERROR(uac_host_interface_shutdown(uac_iface), "Unable to shutdown interface");
        }
        UAC_ENTER_CRITICAL();
    }
    UAC_EXIT_CRITICAL();

    return ESP_OK;
}

/**
 * @brief USB Host Client's event callback
 *
 * @param[in] event    Client event message
 * @param[in] arg      Argument, does not used
 */
static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        _uac_host_device_connected(event->new_dev.address);
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        _uac_host_device_disconnected(event->dev_gone.dev_hdl);
    }
}

/**
 * @brief UAC Host release Interface and free transfers, change state to IDLE
 *
 * @param[in] iface       Pointer to Interface structure,
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_release_and_free_transfer(uac_iface_t *iface)
{
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(iface->parent);

    UAC_RETURN_ON_FALSE(is_interface_in_list(iface), ESP_ERR_NOT_FOUND, "Interface handle not found");
    UAC_RETURN_ON_ERROR(usb_host_interface_release(s_uac_driver->client_handle, iface->parent->dev_hdl, iface->dev_info.iface_num), "Unable to release UAC Interface");

    if (iface->free_xfer_list) {
        for (int i = 0; i < iface->xfer_num; i++) {
            if (iface->free_xfer_list[i]) {
                ESP_ERROR_CHECK(usb_host_transfer_free(iface->free_xfer_list[i]));
            }
        }
        free(iface->free_xfer_list);
    }

    if (iface->xfer_list) {
        for (int i = 0; i < iface->xfer_num; i++) {
            if (iface->xfer_list[i]) {
                ESP_ERROR_CHECK(usb_host_transfer_free(iface->xfer_list[i]));
            }
        }
        free(iface->xfer_list);
    }

    // Change state
    iface->state = UAC_INTERFACE_STATE_IDLE;
    return ESP_OK;
}

/**
 * @brief UAC Host claim Interface and prepare transfer, change state to READY
 *
 * @param[in] iface       Pointer to Interface structure,
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_claim_and_prepare_transfer(uac_iface_t *iface)
{
    esp_err_t ret = ESP_OK;
    // Claim Interface

    UAC_RETURN_ON_ERROR(usb_host_interface_claim(s_uac_driver->client_handle, iface->parent->dev_hdl, iface->dev_info.iface_num,
                        iface->cur_alt + 1), "Unable to claim Interface");
    // alloc a list of usb transfer
    uint32_t packet_size = iface->iface_alt[iface->cur_alt].ep_mps;
    iface->xfer_list = calloc(iface->xfer_num, sizeof(usb_transfer_t *));
    UAC_GOTO_ON_FALSE(iface->xfer_list, ESP_ERR_NO_MEM, "Unable to allocate transfer list");
    iface->free_xfer_list = calloc(iface->xfer_num, sizeof(usb_transfer_t *));
    UAC_GOTO_ON_FALSE(iface->free_xfer_list, ESP_ERR_NO_MEM, "Unable to allocate free transfer list");
    for (int i = 0; i < iface->xfer_num; i++) {
        UAC_GOTO_ON_ERROR(usb_host_transfer_alloc(packet_size * iface->packet_num, iface->packet_num, &iface->free_xfer_list[i]),
                          "Unable to allocate transfer buffer for EP IN");
    }
    // Change state
    iface->state = UAC_INTERFACE_STATE_READY;
    return ESP_OK;

fail:
    uac_host_interface_release_and_free_transfer(iface);
    return ret;
}

/**
 * @brief Control transfer callback for configuration selection
 *
 * @param[in] transfer  Pointer to transfer data structure
 */
static void ctrl_xfer_done_cb(usb_transfer_t *transfer)
{
    assert(transfer);
    SemaphoreHandle_t sem = (SemaphoreHandle_t)transfer->context;
    xSemaphoreGive(sem);
}

/**
 * @brief UAC IN Transfer complete callback
 *
 * @param[in] transfer  Pointer to transfer data structure
 */
static void stream_rx_xfer_done(usb_transfer_t *in_xfer)
{
    assert(in_xfer);

    uac_iface_t *iface = in_xfer->context;
    assert(iface);

    if (iface->state != UAC_INTERFACE_STATE_ACTIVE) {
        in_xfer->status = USB_TRANSFER_STATUS_CANCELED;
    }

    switch (in_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {

        // if ringbuffer will overflow, notify user to read data
        size_t data_len = _ring_buffer_get_len(iface->ringbuf);
        if (data_len + in_xfer->actual_num_bytes >= iface->ringbuf_size) {
            uac_host_user_interface_callback(iface, UAC_HOST_DEVICE_EVENT_RX_DONE);
        }

        // if ringbuffer overflow (happens if user not read in above callback), the data will be dropped
        data_len = _ring_buffer_get_len(iface->ringbuf);
        if (data_len + in_xfer->actual_num_bytes > iface->ringbuf_size) {
            ESP_LOGD(TAG, "RX Ringbuffer overflow");
        } else {
            // else push data to ringbuffer
            for (int i = 0; i < in_xfer->num_isoc_packets; i++) {
                if (in_xfer->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) {
                    // copy data to ringbuffer
                    ESP_LOGD(TAG, "Bad RX Isoc packet %d status %d", i, in_xfer->isoc_packet_desc[i].status);
                    continue;
                }
                int requested_num_bytes = in_xfer->isoc_packet_desc[i].num_bytes;
                int actual_num_bytes = in_xfer->isoc_packet_desc[i].actual_num_bytes;
                // in UAC, the actual_num_bytes may less than requested_num_bytes
                // eg. the packet_size is 64, but the endpoint size is 100
                assert(requested_num_bytes >= actual_num_bytes);
                // copy data to ringbuffer
                _ring_buffer_push(iface->ringbuf, in_xfer->data_buffer + i * requested_num_bytes, actual_num_bytes, 0);
            }
        }
        // Relaunch transfer
        usb_host_transfer_submit(in_xfer);

        // if ringbuffer is reach the threshold, notify user to read out
        data_len = _ring_buffer_get_len(iface->ringbuf);
        if (data_len >= iface->ringbuf_threshold) {
            uac_host_user_interface_callback(iface, UAC_HOST_DEVICE_EVENT_RX_DONE);
        }

        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        // User is notified about device disconnection from usb_event_cb
        // No need to do anything
        return;
    default:
        // Any other error
        break;
    }

    ESP_LOGE(TAG, "Transfer failed, status %d", in_xfer->status);
    // Notify user about transfer or any other error
    uac_host_user_interface_callback(iface, UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR);
}

static void stream_tx_xfer_submit(usb_transfer_t *out_xfer)
{
    uac_iface_t *iface = out_xfer->context;
    assert(iface);

    size_t data_len = _ring_buffer_get_len(iface->ringbuf);
    if (data_len >= iface->packet_size * iface->packet_num) {
        data_len = iface->packet_size * iface->packet_num;
        size_t actual_num_bytes = 0;
        _ring_buffer_pop(iface->ringbuf, out_xfer->data_buffer, data_len, &actual_num_bytes, 0);
        
        // Instead of asserting, handle the case where we got fewer bytes than requested
        if (actual_num_bytes < data_len) {
            // Fill the remaining buffer with zeros to avoid sending garbage
            memset(out_xfer->data_buffer + actual_num_bytes, 0, data_len - actual_num_bytes);
            ESP_LOGW(TAG, "TX buffer underrun: requested %zu bytes, got %zu", data_len, actual_num_bytes);
        }
        
        // Set the actual number of bytes to transfer
        out_xfer->num_bytes = actual_num_bytes;
        
        // Relaunch transfer, as the pipe state may change
        // the transfer may fail eg. the device is disconnected or the pipe is suspended
        // the data in ringbuffer will be dropped without notify user
        usb_host_transfer_submit(out_xfer);
        data_len = _ring_buffer_get_len(iface->ringbuf);
        if (data_len <= iface->ringbuf_threshold) {
            // Notify user send done
            uac_host_user_interface_callback(iface, UAC_HOST_DEVICE_EVENT_TX_DONE);
        }
    } else {
        // add the transfer to free list
        UAC_ENTER_CRITICAL();
        for (int i = 0; i < iface->xfer_num; i++) {
            if (iface->xfer_list[i] == out_xfer) {
                iface->free_xfer_list[i] = out_xfer;
                iface->xfer_list[i] = NULL;
                break;
            }
        }
        UAC_EXIT_CRITICAL();
        // Notify user send done
        uac_host_user_interface_callback(iface, UAC_HOST_DEVICE_EVENT_TX_DONE);
    }
}

/**
 * @brief UAC OUT Transfer complete callback
 *
 * @param[in] transfer  Pointer to transfer data structure
 */
static void stream_tx_xfer_done(usb_transfer_t *out_xfer)
{
    assert(out_xfer);

    uac_iface_t *iface = out_xfer->context;
    assert(iface);

    // If the iface is not active, cancel the transfer
    if (iface->state != UAC_INTERFACE_STATE_ACTIVE) {
        out_xfer->status = USB_TRANSFER_STATUS_CANCELED;
    }

    switch (out_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        // Submit the next transfer
        stream_tx_xfer_submit(out_xfer);
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        // User is notified about device disconnection from usb_event_cb
        // No need to do anything
        return;
    default:
        // Any other error, add the transfer to free list
        UAC_ENTER_CRITICAL();
        for (int i = 0; i < iface->xfer_num; i++) {
            if (iface->xfer_list[i] == out_xfer) {
                iface->free_xfer_list[i] = out_xfer;
                iface->xfer_list[i] = NULL;
                break;
            }
        }
        UAC_EXIT_CRITICAL();
        break;
    }

    ESP_LOGE(TAG, "Transfer failed, status %d", out_xfer->status);
    // Notify user about transfer or any other error
    uac_host_user_interface_callback(iface, UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR);
}

/**
 * @brief Suspend active interface, the interface will be in READY state
 *
 * @param[in] iface       Pointer to Interface structure
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_suspend(uac_iface_t *iface)
{
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(iface->parent);
    UAC_RETURN_ON_INVALID_ARG(iface->free_xfer_list);
    UAC_RETURN_ON_FALSE(is_interface_in_list(iface), ESP_ERR_NOT_FOUND, "Interface handle not found");
    UAC_RETURN_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state), ESP_ERR_INVALID_STATE, "Interface wrong state");

    // Set Interface alternate setting to 0
    usb_setup_packet_t request;
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&request, iface->dev_info.iface_num, 0);
    esp_err_t ret = uac_cs_request_set(iface->parent, (uac_cs_request_t *)&request);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set Interface %d-%d Failed", iface->dev_info.iface_num, 0);
    } else {
        ESP_LOGI(TAG, "Set Interface %d-%d", iface->dev_info.iface_num, 0);
    }

    uint8_t ep_addr = iface->iface_alt[iface->cur_alt].ep_addr;
    UAC_RETURN_ON_ERROR(usb_host_endpoint_halt(iface->parent->dev_hdl, ep_addr), "Unable to HALT EP");
    UAC_RETURN_ON_ERROR(usb_host_endpoint_flush(iface->parent->dev_hdl, ep_addr), "Unable to FLUSH EP");
    usb_host_endpoint_clear(iface->parent->dev_hdl, ep_addr);
    _ring_buffer_flush(iface->ringbuf);

    // add all the transfer to free list
    UAC_ENTER_CRITICAL();
    for (int i = 0; i < iface->xfer_num; i++) {
        if (iface->xfer_list[i]) {
            iface->free_xfer_list[i] = iface->xfer_list[i];
            iface->xfer_list[i] = NULL;
        }
    }
    UAC_EXIT_CRITICAL();
    // Change state
    iface->state = UAC_INTERFACE_STATE_READY;

    return ESP_OK;
}

/**
 * @brief Resume suspended interface, the interface will be in ACTIVE state
 *
 * @param[in] iface       Pointer to Interface structure
 * @return esp_err_t
 */
static esp_err_t uac_host_interface_resume(uac_iface_t *iface)
{
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(iface->parent);
    UAC_RETURN_ON_INVALID_ARG(iface->free_xfer_list);
    UAC_RETURN_ON_FALSE(is_interface_in_list(iface), ESP_ERR_NOT_FOUND, "Interface handle not found");
    UAC_RETURN_ON_FALSE((UAC_INTERFACE_STATE_READY == iface->state), ESP_ERR_INVALID_STATE, "Interface wrong state");

    // Set Interface alternate setting
    usb_setup_packet_t request;
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&request, iface->dev_info.iface_num, iface->cur_alt + 1);
    UAC_RETURN_ON_ERROR(uac_cs_request_set(iface->parent, (uac_cs_request_t *)&request), "Unable to set Interface alternate");
    ESP_LOGI(TAG, "Set Interface %d-%d", iface->dev_info.iface_num, iface->cur_alt + 1);
    // Set endpoint frequency control
    if (iface->iface_alt[iface->cur_alt].freq_ctrl_supported) {
        ESP_LOGI(TAG, "Set EP %02X frequency %"PRIu32, iface->iface_alt[iface->cur_alt].ep_addr, iface->iface_alt[iface->cur_alt].cur_sampling_freq);
        UAC_RETURN_ON_ERROR(uac_cs_request_set_ep_frequency(iface, iface->iface_alt[iface->cur_alt].ep_addr,
                            iface->iface_alt[iface->cur_alt].cur_sampling_freq), "Unable to set endpoint frequency");
    }
    // for RX, we just submit all the transfers
    if (iface->dev_info.type == UAC_STREAM_RX) {
        assert(iface->iface_alt[iface->cur_alt].ep_addr & 0x80);
        for (int i = 0; i < iface->xfer_num; i++) {
            assert(iface->free_xfer_list[i]);
            iface->free_xfer_list[i]->device_handle = iface->parent->dev_hdl;
            iface->free_xfer_list[i]->callback = stream_rx_xfer_done;
            iface->free_xfer_list[i]->context = iface;
            iface->free_xfer_list[i]->timeout_ms = DEFAULT_ISOC_XFER_TIMEOUT_MS;
            iface->free_xfer_list[i]->bEndpointAddress = iface->iface_alt[iface->cur_alt].ep_addr;
            // we request the size same as the MPS of the endpoint, but the actual size should be checked in the callback
            iface->free_xfer_list[i]->num_bytes = iface->iface_alt[iface->cur_alt].ep_mps * iface->packet_num;
            // set request nub_bytes of each packet
            for (int j = 0; j < iface->packet_num; j++) {
                iface->free_xfer_list[i]->isoc_packet_desc[j].num_bytes = iface->iface_alt[iface->cur_alt].ep_mps;
            }
            iface->xfer_list[i] = iface->free_xfer_list[i];
            iface->free_xfer_list[i] = NULL;
            UAC_RETURN_ON_ERROR(usb_host_transfer_submit(iface->xfer_list[i]), "Unable to submit RX transfer");
        }
    } else if (iface->dev_info.type == UAC_STREAM_TX) {
        assert(!(iface->iface_alt[iface->cur_alt].ep_addr & 0x80));
        // for TX, we submit the first transfer with data 0 to make the speaker quiet
        for (int i = 0; i < iface->xfer_num; i++) {
            assert(iface->free_xfer_list[i]);
            iface->free_xfer_list[i]->device_handle = iface->parent->dev_hdl;
            iface->free_xfer_list[i]->callback = stream_tx_xfer_done;
            iface->free_xfer_list[i]->context = iface;
            iface->free_xfer_list[i]->timeout_ms = DEFAULT_ISOC_XFER_TIMEOUT_MS;
            iface->free_xfer_list[i]->bEndpointAddress = iface->iface_alt[iface->cur_alt].ep_addr;
            // set the data buffer to 0
            memset(iface->free_xfer_list[i]->data_buffer, 0, iface->free_xfer_list[i]->data_buffer_size);
            // for synchronous transfer type, the packet size depends on the actual sample rate, channels and bit resolution.
            for (int j = 0; j < iface->packet_num; j++) {
                iface->free_xfer_list[i]->isoc_packet_desc[j].num_bytes = iface->packet_size;
            }
            iface->free_xfer_list[i]->num_bytes = iface->packet_num * iface->packet_size;
        }
    }

    // for TX, we check if data is available in the ringbuffer, if yes, we submit the transfer
    iface->state = UAC_INTERFACE_STATE_ACTIVE;

    return ESP_OK;
}

/**
 * @brief Add UAC physical device to the list
 * @param[in] addr          USB device address
 * @param[in] dev_hdl       USB device handle
 * @param[in] config_desc   Pointer to Configuration Descriptor
 * @param[out] uac_device_handle  Pointer to UAC device handle
 * @return esp_err_t
 */
static esp_err_t _uac_host_device_add(uint8_t addr, usb_device_handle_t dev_hdl, const usb_config_desc_t *config_desc, uac_device_t **uac_device_handle)
{
    assert(config_desc);
    esp_err_t ret;
    uac_device_t *uac_device;
    UAC_GOTO_ON_FALSE(uac_device = calloc(1, sizeof(uac_device_t)), ESP_ERR_NO_MEM, "Unable to allocate memory for UAC Device");

    uac_device->addr = addr;
    uac_device->dev_hdl = dev_hdl;
    
    // Check for device quirks
    const usb_device_desc_t *dev_desc;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(dev_hdl, &dev_desc));
    
    // Check for configuration quirks
    uac_quirk_t quirk = {
        .type = UAC_QUIRK_FIXED_CONFIG
    };
    
    if (uac_check_quirks(dev_desc, &quirk) &&
        quirk.type == UAC_QUIRK_FIXED_CONFIG &&
        quirk.config_num != config_desc->bConfigurationValue) {
        
        ESP_LOGI(TAG, "Applying quirk: Forcing configuration %d", quirk.config_num);
        
        // Set the configuration
        usb_setup_packet_t setup;
        USB_SETUP_PACKET_INIT_SET_CONFIG(&setup, quirk.config_num);
        
        // Create a semaphore for the control transfer
        SemaphoreHandle_t ctrl_xfer_done = xSemaphoreCreateBinary();
        if (!ctrl_xfer_done) {
            ESP_LOGE(TAG, "Failed to create semaphore");
            free(uac_device);
            return ESP_ERR_NO_MEM;
        }
        
        // Create a transfer for the setup packet
        usb_transfer_t *transfer;
        ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &transfer));
        transfer->device_handle = dev_hdl;
        transfer->bEndpointAddress = 0;
        transfer->callback = ctrl_xfer_done_cb;
        transfer->context = ctrl_xfer_done;
        transfer->timeout_ms = 1000;
        transfer->num_bytes = sizeof(usb_setup_packet_t);
        memcpy(transfer->data_buffer, &setup, sizeof(usb_setup_packet_t));
        
        // Submit the transfer
        esp_err_t err = usb_host_transfer_submit_control(s_uac_driver->client_handle, transfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set configuration %d: %s", quirk.config_num, esp_err_to_name(err));
            usb_host_transfer_free(transfer);
            vSemaphoreDelete(ctrl_xfer_done);
            free(uac_device);
            return ESP_ERR_NOT_SUPPORTED;
        }
        
        // Wait for the transfer to complete
        if (xSemaphoreTake(ctrl_xfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Control transfer timeout");
            usb_host_transfer_free(transfer);
            vSemaphoreDelete(ctrl_xfer_done);
            free(uac_device);
            return ESP_ERR_TIMEOUT;
        }
        
        // Check if the transfer was successful
        if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGW(TAG, "Control transfer failed with status %d", transfer->status);
            usb_host_transfer_free(transfer);
            vSemaphoreDelete(ctrl_xfer_done);
            free(uac_device);
            return ESP_ERR_INVALID_RESPONSE;
        }
        
        // Free the transfer and semaphore
        usb_host_transfer_free(transfer);
        vSemaphoreDelete(ctrl_xfer_done);
        
        // Get the new configuration descriptor
        const usb_config_desc_t *new_config_desc;
        err = usb_host_get_active_config_descriptor(dev_hdl, &new_config_desc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get new configuration descriptor: %s", esp_err_to_name(err));
            free(uac_device);
            return err;
        }
        
        // Use the new configuration descriptor
        config_desc = new_config_desc;
        ESP_LOGI(TAG, "Successfully switched to configuration %d", quirk.config_num);
    }

    // Parse the configuration descriptor to find the UAC control interface
    const size_t total_length = config_desc->wTotalLength;
    const usb_intf_desc_t *iface_desc = NULL;
    const uac_desc_header_t *uac_cs_desc = NULL;
    int iface_offset = 0;
    int uac_desc_offset = 0;
    // Get first Interface descriptor
    iface_desc = GET_NEXT_INTERFACE_DESC(config_desc, total_length, iface_offset);
    while (iface_desc != NULL) {
        uac_desc_offset = iface_offset;
        // 1. Parse the configuration descriptor to find only the first the UAC control interface
        // 2. Parse each class specific audio control interface
        // 2.1. for header descriptor, check if it is supported uac version
        // 2.2. save the class specific audio control descriptor for future use
        if (iface_desc->bInterfaceClass == USB_CLASS_AUDIO && iface_desc->bInterfaceSubClass == UAC_SUBCLASS_AUDIOCONTROL) {
            ESP_LOGD(TAG, "Found UAC Control, bInterfaceNumber=%d", iface_desc->bInterfaceNumber);
            uac_cs_desc = GET_NEXT_UAC_CS_DESC(iface_desc, total_length, uac_desc_offset);
            uac_device->ctrl_iface_num = iface_desc->bInterfaceNumber;
            // break if all UAC control class specific descriptors are parsed
            while (uac_cs_desc != NULL) {
                switch (uac_cs_desc->bDescriptorSubtype) {
                case UAC_AC_HEADER: {
                    static bool selecting_config = false;
                    const uac_ac_header_desc_t *header_desc = (const uac_ac_header_desc_t *)uac_cs_desc;
                    
                    if (header_desc->bcdADC != 0x0100 && !selecting_config) {
                        ESP_LOGW(TAG, "UAC version %04X detected, trying to select a compatible configuration", header_desc->bcdADC);
                        
                        // Get the number of configurations
                        const usb_device_desc_t *dev_desc;
                        ESP_ERROR_CHECK(usb_host_get_device_descriptor(dev_hdl, &dev_desc));
                        uint8_t num_configs = dev_desc->bNumConfigurations;
                        
                        // If there's only one configuration, we can't do anything
                        if (num_configs <= 1) {
                            ESP_LOGW(TAG, "Device only has one configuration, cannot select a different one");
                            free(uac_device);
                            return ESP_ERR_NOT_SUPPORTED;
                        }
                        
                        // Create a semaphore for the control transfer
                        SemaphoreHandle_t ctrl_xfer_done = xSemaphoreCreateBinary();
                        if (!ctrl_xfer_done) {
                            ESP_LOGE(TAG, "Failed to create semaphore");
                            free(uac_device);
                            return ESP_ERR_NO_MEM;
                        }
                        
                        // Set flag to prevent recursion
                        selecting_config = true;
                        
                        // Try to find a configuration that doesn't use UAC 2.0 protocol
                        for (uint8_t i = 0; i < num_configs; i++) {
                            // Skip the current configuration
                            if (i == config_desc->bConfigurationValue - 1) {
                                continue;
                            }
                            
                            ESP_LOGI(TAG, "Trying configuration %d", i + 1);
                            
                            // Set the configuration
                            usb_setup_packet_t setup;
                            USB_SETUP_PACKET_INIT_SET_CONFIG(&setup, i + 1);
                            
                            // Create a transfer for the setup packet
                            usb_transfer_t *transfer;
                            ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &transfer));
                            transfer->device_handle = dev_hdl;
                            transfer->bEndpointAddress = 0;
                            transfer->callback = ctrl_xfer_done_cb;
                            transfer->context = ctrl_xfer_done;
                            transfer->timeout_ms = 1000;
                            transfer->num_bytes = sizeof(usb_setup_packet_t);
                            memcpy(transfer->data_buffer, &setup, sizeof(usb_setup_packet_t));
                            
                            // Submit the transfer
                            esp_err_t err = usb_host_transfer_submit_control(s_uac_driver->client_handle, transfer);
                            if (err != ESP_OK) {
                                ESP_LOGW(TAG, "Failed to set configuration %d: %s", i + 1, esp_err_to_name(err));
                                usb_host_transfer_free(transfer);
                                continue;
                            }
                            
                            // Wait for the transfer to complete
                            if (xSemaphoreTake(ctrl_xfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
                                ESP_LOGW(TAG, "Control transfer timeout");
                                usb_host_transfer_free(transfer);
                                continue;
                            }
                            
                            // Check if the transfer was successful
                            if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
                                ESP_LOGW(TAG, "Control transfer failed with status %d", transfer->status);
                                usb_host_transfer_free(transfer);
                                continue;
                            }
                            
                            // Free the transfer
                            usb_host_transfer_free(transfer);
                            
                            // Get the new configuration descriptor
                            const usb_config_desc_t *new_config_desc;
                            err = usb_host_get_active_config_descriptor(dev_hdl, &new_config_desc);
                            if (err != ESP_OK) {
                                ESP_LOGW(TAG, "Failed to get new configuration descriptor: %s", esp_err_to_name(err));
                                continue;
                            }
                            
                            // Try to add the device with the new configuration
                            uac_device_t *new_device = NULL;
                            err = _uac_host_device_add(addr, dev_hdl, new_config_desc, &new_device);
                            if (err == ESP_OK) {
                                // Successfully added the device with the new configuration
                                ESP_LOGI(TAG, "Successfully selected configuration %d", i + 1);
                                *uac_device_handle = new_device;
                                selecting_config = false;
                                return ESP_OK;
                            }
                        }
                        
                        // If we get here, we couldn't find a compatible configuration
                        ESP_LOGW(TAG, "Could not find a compatible configuration");
                        selecting_config = false;
                        free(uac_device);
                        return ESP_ERR_NOT_SUPPORTED;
                    }
                    uint8_t *cs_ac_desc = calloc(header_desc->wTotalLength, sizeof(uint8_t));
                    UAC_GOTO_ON_FALSE(cs_ac_desc, ESP_ERR_NO_MEM, "Unable to allocate memory for UAC Control CS descriptor");
                    memcpy(cs_ac_desc, uac_cs_desc, header_desc->wTotalLength);
                    uac_device->cs_ac_desc = cs_ac_desc;
                    ESP_LOGD(TAG, "UAC version %04X", header_desc->bcdADC);
                    break;
                }
                default:
                    break;
                }
                uac_cs_desc = (uac_desc_header_t *)GET_NEXT_DESC(uac_cs_desc, total_length, uac_desc_offset);
                if (uac_cs_desc->bDescriptorType != UAC_CS_INTERFACE) {
                    // parse stop if the next descriptor is not UAC class specific interface descriptor
                    break;
                }
            }
            // all parse stop, only parse the first UAC control interface
            break;
        }
        iface_desc = GET_NEXT_INTERFACE_DESC(iface_desc, total_length, iface_offset);
    }

    // Create Semaphore for control transfer
    UAC_GOTO_ON_FALSE(uac_device->ctrl_xfer_done = xSemaphoreCreateBinary(), ESP_ERR_NO_MEM, "Unable to create semaphore");
    UAC_GOTO_ON_FALSE(uac_device->device_busy =  xSemaphoreCreateMutex(), ESP_ERR_NO_MEM, "Unable to create mutex");

    // Allocate control transfer buffer
    UAC_GOTO_ON_ERROR(usb_host_transfer_alloc(64, 0, &uac_device->ctrl_xfer), "Unable to allocate transfer buffer");

    UAC_GOTO_ON_FALSE_CRITICAL(s_uac_driver, ESP_ERR_INVALID_STATE);
    UAC_GOTO_ON_FALSE_CRITICAL(s_uac_driver->client_handle, ESP_ERR_INVALID_STATE);
    UAC_ENTER_CRITICAL();
    STAILQ_INSERT_TAIL(&s_uac_driver->uac_devices_tailq, uac_device, tailq_entry);
    UAC_EXIT_CRITICAL();

    if (uac_device_handle) {
        *uac_device_handle = uac_device;
    }

    return ESP_OK;

fail:
    _uac_host_device_delete(uac_device);
    return ret;
}

/**
 * @brief Remove UAC physical device from the list
 * @param[in] uac_device    Pointer to UAC device structure
 * @return esp_err_t
 */
static esp_err_t _uac_host_device_delete(uac_device_t *uac_device)
{
    UAC_RETURN_ON_INVALID_ARG(uac_device);

    if (uac_device->ctrl_xfer) {
        UAC_RETURN_ON_ERROR(usb_host_transfer_free(uac_device->ctrl_xfer), "Unable to free transfer buffer for EP0");
    }

    if (uac_device->ctrl_xfer_done) {
        vSemaphoreDelete(uac_device->ctrl_xfer_done);
    }

    if (uac_device->device_busy) {
        vSemaphoreDelete(uac_device->device_busy);
    }

    if (uac_device->cs_ac_desc) {
        free(uac_device->cs_ac_desc);
    }

    ESP_LOGD(TAG, "Remove addr %d device from list", uac_device->addr);

    UAC_ENTER_CRITICAL();
    STAILQ_REMOVE(&s_uac_driver->uac_devices_tailq, uac_device, uac_host_device, tailq_entry);
    UAC_EXIT_CRITICAL();

    free(uac_device);
    return ESP_OK;
}

/** Lock UAC device from other task
 *
 * @param[in] uac_device    Pointer to UAC device structure
 * @param[in] timeout_ms    Timeout of trying to take the mutex
 * @return esp_err_t
 */
static inline esp_err_t _uac_host_device_try_lock(uac_device_t *uac_device, uint32_t timeout_ms)
{
    return (xSemaphoreTake(uac_device->device_busy, pdMS_TO_TICKS(timeout_ms)) ? ESP_OK : ESP_ERR_TIMEOUT);
}

/** Unlock UAC device from other task
 *
 * @param[in] uac_device    Pointer to UAC device structure
 * @param[in] timeout_ms    Timeout of trying to take the mutex
 * @return esp_err_t
 */
static inline void _uac_host_device_unlock(uac_device_t *uac_device)
{
    xSemaphoreGive(uac_device->device_busy);
}

/**
 * @brief UAC Control transfer complete callback
 *
 * @param[in] ctrl_xfer  Pointer to transfer data structure
 */
static void ctrl_xfer_done(usb_transfer_t *ctrl_xfer)
{
    assert(ctrl_xfer);
    uac_device_t *uac_device = (uac_device_t *)ctrl_xfer->context;
    xSemaphoreGive(uac_device->ctrl_xfer_done);
}

/**
 * @brief UAC control transfer synchronous.
 *
 * @param[in] uac_device  Pointer to UAC device structure
 * @param[in] len         Number of bytes to transfer
 * @param[in] timeout_ms  Timeout in ms
 * @return esp_err_t
 */
static esp_err_t uac_control_transfer(uac_device_t *uac_device, int len, uint32_t timeout_ms)
{

    usb_transfer_t *ctrl_xfer = uac_device->ctrl_xfer;

    ctrl_xfer->device_handle = uac_device->dev_hdl;
    ctrl_xfer->callback = ctrl_xfer_done;
    ctrl_xfer->context = uac_device;
    ctrl_xfer->bEndpointAddress = 0;
    ctrl_xfer->timeout_ms = timeout_ms;
    ctrl_xfer->num_bytes = len;

    UAC_RETURN_ON_ERROR(usb_host_transfer_submit_control(s_uac_driver->client_handle, ctrl_xfer), "Unable to submit control transfer");

    BaseType_t received = xSemaphoreTake(uac_device->ctrl_xfer_done, pdMS_TO_TICKS(ctrl_xfer->timeout_ms));

    if (received != pdTRUE) {
        // Transfer was not finished, error in USB LIB. Reset the endpoint
        ESP_LOGE(TAG, "Control Transfer Timeout");
        UAC_RETURN_ON_ERROR(usb_host_endpoint_halt(uac_device->dev_hdl, ctrl_xfer->bEndpointAddress), "Unable to HALT EP");
        UAC_RETURN_ON_ERROR(usb_host_endpoint_flush(uac_device->dev_hdl, ctrl_xfer->bEndpointAddress), "Unable to FLUSH EP");
        usb_host_endpoint_clear(uac_device->dev_hdl, ctrl_xfer->bEndpointAddress);
        return ESP_ERR_TIMEOUT;
    }

    UAC_RETURN_ON_FALSE(ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, "Control transfer error");
    UAC_RETURN_ON_FALSE(ctrl_xfer->actual_num_bytes <= ctrl_xfer->num_bytes, ESP_ERR_INVALID_RESPONSE, "Incorrect number of bytes transferred");
    ESP_LOG_BUFFER_HEXDUMP(TAG, ctrl_xfer->data_buffer, ctrl_xfer->actual_num_bytes, ESP_LOG_DEBUG);

    return ESP_OK;
}

/**
 * @brief UAC class specific request Set
 *
 * @param[in] uac_device Pointer to UAC device structure
 * @param[in] req        Pointer to a class specific request structure
 * @return esp_err_t
 */
static esp_err_t uac_cs_request_set(uac_device_t *uac_device, const uac_cs_request_t *req)
{
    esp_err_t ret;
    usb_transfer_t *ctrl_xfer = uac_device->ctrl_xfer;
    UAC_RETURN_ON_INVALID_ARG(uac_device);
    UAC_RETURN_ON_INVALID_ARG(uac_device->ctrl_xfer);

    UAC_RETURN_ON_ERROR(_uac_host_device_try_lock(uac_device, DEFAULT_CTRL_XFER_TIMEOUT_MS), "UAC Device is busy by other task");

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer->data_buffer;

    setup->bmRequestType = req->bmRequestType;
    setup->bRequest = req->bRequest;
    setup->wValue = req->wValue;
    setup->wIndex = req->wIndex;
    setup->wLength = req->wLength;

    if (setup->bmRequestType == 0) {
        setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS
                               | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    }

    if (req->wLength && req->data) {
        memcpy(ctrl_xfer->data_buffer + USB_SETUP_PACKET_SIZE, req->data, req->wLength);
    }

    ret = uac_control_transfer(uac_device, USB_SETUP_PACKET_SIZE + setup->wLength, DEFAULT_CTRL_XFER_TIMEOUT_MS);

    _uac_host_device_unlock(uac_device);

    return ret;
}

/**
 * @brief UAC class specific request - Set Endpoint Frequency
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] ep_addr     Endpoint address
 * @param[in] freq        Frequency to set
 * @return esp_err_t
 */
static esp_err_t uac_cs_request_set_ep_frequency(uac_iface_t *iface, uint8_t ep_addr, uint32_t freq)
{
    uint8_t tmp[3] = { 0, 0, 0 };

    const uac_cs_request_t set_freq = {
        .bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS
        | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT,
        .bRequest = UAC_SET_CUR,
        .wValue = UAC_SAMPLING_FREQ_CONTROL << 8,
                                            .wIndex = (0x00ff & ep_addr),
                                            .wLength = 3,
                                            .data = tmp
    };

    tmp[0] = freq & 0xff;
    tmp[1] = (freq >> 8) & 0xff;
    tmp[2] = (freq >> 16) & 0xff;
    return uac_cs_request_set(iface->parent, &set_freq);
}

/**
 * @brief UAC class specific request - Set Volume
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] volume      Volume to set, db
 * @return esp_err_t
 */
static esp_err_t uac_cs_request_set_volume(uac_iface_t *iface, uint32_t volume)
{
    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;
    uint8_t vol_ch_map = iface->iface_alt[iface->cur_alt].vol_ch_map;
    UAC_RETURN_ON_FALSE(feature_unit && vol_ch_map, ESP_ERR_NOT_SUPPORTED, "volume control not supported");
    uint8_t ctrl_iface_num = iface->parent->ctrl_iface_num;
    uint8_t tmp[2] = { 0, 0 };
    esp_err_t ret = ESP_OK;

    uac_cs_request_t set_volume = {
        .bRequest = UAC_SET_CUR,
        .wIndex = (feature_unit << 8) | (ctrl_iface_num & 0xff),
        .wLength = 2,
        .data = tmp
    };

    tmp[0] = volume & 0xff;
    tmp[1] = (volume >> 8) & 0xff;

    // set volume for all logical channels
    // we not support separate volume control for each channel
    for (size_t i = 0; i < 8; i++) {
        if (vol_ch_map & (1 << i)) {
            set_volume.wValue = (UAC_VOLUME_CONTROL << 8) | i,
            ret = uac_cs_request_set(iface->parent, &set_volume);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }
    ESP_LOGD(TAG, "Set volume 0x%04X db", (unsigned int)volume);
    return ret;
}

/**
 * @brief UAC class specific request - Set Mute
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] mute        True to mute, false to unmute
 * @return esp_err_t
 */
static esp_err_t uac_cs_request_set_mute(uac_iface_t *iface, bool mute)
{
    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;
    uint8_t mute_ch_map = iface->iface_alt[iface->cur_alt].mute_ch_map;
    UAC_RETURN_ON_FALSE(feature_unit, ESP_ERR_NOT_SUPPORTED, "mute control not supported");
    uint8_t ctrl_iface_num = iface->parent->ctrl_iface_num;
    uint8_t tmp[1] = { 0 };
    esp_err_t ret = ESP_OK;

    uac_cs_request_t set_mute = {
        .bRequest = UAC_SET_CUR,
        .wIndex = (feature_unit << 8) | (ctrl_iface_num & 0xff),
        .wLength = 1,
        .data = tmp
    };

    tmp[0] = mute;

    // set mute for all logical channels
    // we not support separate mute control for each channel
    for (size_t i = 0; i < 8; i++) {
        if (mute_ch_map & (1 << i)) {
            set_mute.wValue = (UAC_MUTE_CONTROL << 8) | i,
            ret = uac_cs_request_set(iface->parent, &set_mute);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }
    ESP_LOGD(TAG, "Set mute %d", mute);
    return ret;
}

esp_err_t uac_host_install(const uac_host_driver_config_t *config)
{
    esp_err_t ret;
    // Make sure uac driver is not installed
    UAC_RETURN_ON_FALSE(!s_uac_driver, ESP_ERR_INVALID_STATE, "UAC Host driver is already installed");
    UAC_RETURN_ON_INVALID_ARG(config);
    UAC_RETURN_ON_INVALID_ARG(config->callback);

    if (config->create_background_task) {
        UAC_RETURN_ON_FALSE(config->stack_size != 0, ESP_ERR_INVALID_ARG, "Wrong stack size value");
        UAC_RETURN_ON_FALSE(config->task_priority != 0, ESP_ERR_INVALID_ARG, "Wrong task priority value");
        UAC_RETURN_ON_FALSE(config->core_id < 2, ESP_ERR_INVALID_ARG, "Wrong core id value");
    }

    uac_driver_t *driver = heap_caps_calloc(1, sizeof(uac_driver_t), MALLOC_CAP_DEFAULT);
    UAC_RETURN_ON_FALSE(driver, ESP_ERR_NO_MEM, "Unable to allocate memory");

    driver->user_cb = config->callback;
    driver->user_arg = config->callback_arg;
    driver->end_client_event_handling = false;
    driver->all_events_handled = xSemaphoreCreateBinary();
    UAC_GOTO_ON_FALSE(driver->all_events_handled, ESP_ERR_NO_MEM, "Unable to create semaphore");

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .async.client_event_callback = client_event_cb,
        .async.callback_arg = NULL,
        .max_num_event_msg = 16,
    };
    UAC_GOTO_ON_ERROR(usb_host_client_register(&client_config, &driver->client_handle), "Unable to register USB Host client");

    UAC_ENTER_CRITICAL();
    s_uac_driver = driver;
    STAILQ_INIT(&s_uac_driver->uac_devices_tailq);
    STAILQ_INIT(&s_uac_driver->uac_ifaces_tailq);
    UAC_EXIT_CRITICAL();

    if (config->create_background_task) {
        BaseType_t task_created = xTaskCreatePinnedToCore(event_handler_task, "USB UAC Host", config->stack_size,
                                  NULL, config->task_priority, NULL, config->core_id);
        UAC_GOTO_ON_FALSE(task_created, ESP_ERR_NO_MEM, "Unable to create USB UAC Host task");
    }

    return ESP_OK;

fail:
    s_uac_driver = NULL;
    if (driver->client_handle) {
        usb_host_client_deregister(driver->client_handle);
    }
    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    free(driver);
    return ret;
}

esp_err_t uac_host_uninstall(void)
{
    // Make sure uac driver is installed,
    UAC_RETURN_ON_FALSE(s_uac_driver, ESP_OK, "UAC Host driver was not installed");

    // Make sure that no uac device is registered
    UAC_ENTER_CRITICAL();
    UAC_RETURN_ON_FALSE_CRITICAL(!s_uac_driver->end_client_event_handling, ESP_ERR_INVALID_STATE);
    // assert that not all devices and interfaces are closed before uninstalling
    UAC_RETURN_ON_FALSE_CRITICAL(STAILQ_EMPTY(&s_uac_driver->uac_devices_tailq), ESP_ERR_INVALID_STATE);
    UAC_RETURN_ON_FALSE_CRITICAL(STAILQ_EMPTY(&s_uac_driver->uac_ifaces_tailq), ESP_ERR_INVALID_STATE);
    s_uac_driver->end_client_event_handling = true;
    UAC_EXIT_CRITICAL();

    if (s_uac_driver->event_handling_started) {
        ESP_ERROR_CHECK(usb_host_client_unblock(s_uac_driver->client_handle));
        // In case the event handling started, we must wait until it finishes
        xSemaphoreTake(s_uac_driver->all_events_handled, portMAX_DELAY);
    }
    vSemaphoreDelete(s_uac_driver->all_events_handled);
    ESP_ERROR_CHECK(usb_host_client_deregister(s_uac_driver->client_handle));
    free(s_uac_driver);
    s_uac_driver = NULL;
    return ESP_OK;
}

esp_err_t uac_host_device_open(const uac_host_device_config_t *config, uac_host_device_handle_t *uac_dev_handle)
{
    UAC_RETURN_ON_INVALID_ARG(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(config);
    *uac_dev_handle = NULL;

    UAC_RETURN_ON_FALSE(s_uac_driver, ESP_ERR_INVALID_STATE, "UAC Driver is not installed");
    UAC_RETURN_ON_FALSE(config->addr, ESP_ERR_INVALID_ARG, "Invalid device address");
    UAC_RETURN_ON_FALSE(config->iface_num, ESP_ERR_INVALID_ARG, "Invalid interface number");
    UAC_RETURN_ON_FALSE(config->buffer_size, ESP_ERR_INVALID_ARG, "Invalid buffer size");
    UAC_RETURN_ON_FALSE(config->buffer_size > config->buffer_threshold, ESP_ERR_INVALID_ARG, "Invalid buffer threshold");

    ESP_LOGD(TAG, "Open Device addr %d, iface %d", config->addr, config->iface_num);
    // Check if the logic device/interface is already added
    uac_iface_t *uac_iface = get_interface_by_addr(config->addr, config->iface_num);
    if (uac_iface) {
        *uac_dev_handle = (uac_host_device_handle_t)uac_iface;
        ESP_LOGD(TAG, "Device addr %d, iface %d already opened", config->addr, config->iface_num);
        return ESP_OK;
    }

    // Check if the physical device is already added
    esp_err_t ret;
    uac_device_t *uac_device = get_uac_device_by_addr(config->addr);
    bool new_device = false;
    usb_device_handle_t dev_hdl = NULL;
    if (!uac_device) {
        UAC_GOTO_ON_ERROR(usb_host_device_open(s_uac_driver->client_handle, config->addr, &dev_hdl), "Unable to open USB device");
        ESP_LOGD(TAG, "line %d, Open Device addr %d", __LINE__, config->addr);
        const usb_config_desc_t *config_desc;
        UAC_GOTO_ON_ERROR(usb_host_get_active_config_descriptor(dev_hdl, &config_desc), "Unable to get active config descriptor");
        // Proceed, add UAC device to the list, parse device params from descriptors
        UAC_GOTO_ON_ERROR(_uac_host_device_add(config->addr, dev_hdl, config_desc, &uac_device), "Unable to add UAC device");
        new_device = true;
        uac_device->opened_cnt = 0;
    }

    UAC_GOTO_ON_ERROR(uac_host_interface_add(uac_device, config->iface_num, &uac_iface), "Unable to add interface");

    // Save UAC Interface callback
    uac_iface->user_cb = config->callback;
    uac_iface->user_cb_arg = config->callback_arg;
    // create a ringbuffer for the incoming/outgoing data
    uac_iface->ringbuf = xRingbufferCreate(config->buffer_size, RINGBUF_TYPE_BYTEBUF);
    UAC_GOTO_ON_FALSE(uac_iface->ringbuf, ESP_ERR_NO_MEM, "Unable to create ringbuffer");
    uac_iface->ringbuf_size = config->buffer_size;
    // if the threshold is not set, set it to 25% of the buffer size
    uac_iface->ringbuf_threshold = config->buffer_threshold ? config->buffer_threshold : config->buffer_size / 4;
    uac_iface->state = UAC_INTERFACE_STATE_IDLE;
    *uac_dev_handle = (uac_host_device_handle_t)uac_iface;
    UAC_ENTER_CRITICAL();
    uac_device->opened_cnt++;
    UAC_EXIT_CRITICAL();
    return ESP_OK;

fail:
    if (uac_iface) {
        uac_host_interface_delete(uac_iface);
    }
    if (new_device) {
        _uac_host_device_delete(uac_device);
    }
    if (dev_hdl) {
        usb_host_device_close(s_uac_driver->client_handle, dev_hdl);
    }
    if (uac_iface->ringbuf) {
        vRingbufferDelete(uac_iface->ringbuf);
    }
    return ret;
}

esp_err_t uac_host_device_close(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *uac_iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(uac_iface);

    esp_err_t ret = ESP_OK;
    ESP_LOGD(TAG, "Close addr %d, iface %d, state %d", uac_iface->dev_info.addr, uac_iface->dev_info.iface_num, uac_iface->state);

    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(uac_iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "UAC Interface is busy by other task");
    if (UAC_INTERFACE_STATE_ACTIVE == uac_iface->state) {
        UAC_GOTO_ON_ERROR(uac_host_interface_suspend(uac_iface), "Unable to disable UAC Interface");
    }

    if (UAC_INTERFACE_STATE_READY == uac_iface->state) {
        UAC_GOTO_ON_ERROR(uac_host_interface_release_and_free_transfer(uac_iface), "Unable to release UAC Interface");
    }

    // Wait for user delete
    if (FLAG_INTERFACE_WAIT_USER_DELETE & uac_iface->flags) {
        uac_host_interface_unlock(uac_iface);
        return ESP_OK;
    }

    UAC_ENTER_CRITICAL();
    if (--uac_iface->parent->opened_cnt == 0) {
        UAC_EXIT_CRITICAL();
        UAC_GOTO_ON_ERROR(usb_host_device_close(s_uac_driver->client_handle, uac_iface->parent->dev_hdl), "Unable to close USB device");
        ESP_LOGD(TAG, "line %d, Close Device addr %d", __LINE__, uac_iface->parent->addr);
        UAC_GOTO_ON_ERROR(_uac_host_device_delete(uac_iface->parent), "Unable to delete UAC device");
        uac_iface->parent = NULL;
        UAC_ENTER_CRITICAL();
    }
    UAC_EXIT_CRITICAL();

    // To delete the ringbuffer safely
    // We should unblock the task that is waiting for the ringbuffer
    if (uac_iface->ringbuf) {
        if (uac_iface->dev_info.type == UAC_STREAM_RX) {
            // Unblock the task that is waiting for read from the ringbuffer
            uint8_t dummy = 0;
            xRingbufferSend(uac_iface->ringbuf, &dummy, sizeof(dummy), 0);
        } else {
            // Unblock the task that is waiting for the ringbuffer
            _ring_buffer_flush(uac_iface->ringbuf);
        }
        // Wait 10 ms for low priority task to unblock
        vTaskDelay(pdMS_TO_TICKS(10));
        vRingbufferDelete(uac_iface->ringbuf);
        uac_iface->ringbuf = NULL;
    }

    uac_iface->user_cb = NULL;
    uac_iface->user_cb_arg = NULL;
    ESP_LOGD(TAG, "User Remove addr %d, iface %d from list", uac_iface->dev_info.addr, uac_iface->dev_info.iface_num);
    uac_host_interface_delete(uac_iface);

    return ESP_OK;

fail:
    uac_host_interface_unlock(uac_iface);
    return ret;
}

esp_err_t uac_host_handle_events(uint32_t timeout)
{
    UAC_RETURN_ON_FALSE(s_uac_driver != NULL, ESP_ERR_INVALID_STATE, "UAC Driver is not installed");
    s_uac_driver->event_handling_started = true;
    esp_err_t ret = usb_host_client_handle_events(s_uac_driver->client_handle, timeout);
    UAC_ENTER_CRITICAL();
    if (s_uac_driver->end_client_event_handling) {
        UAC_EXIT_CRITICAL();
        xSemaphoreGive(s_uac_driver->all_events_handled);
        return ESP_FAIL;
    }
    UAC_EXIT_CRITICAL();
    return ret;
}

esp_err_t uac_host_get_device_alt_param(uac_host_device_handle_t uac_dev_handle, uint8_t iface_alt, uac_host_dev_alt_param_t *uac_alt_param)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_FALSE(iface, ESP_ERR_INVALID_STATE, "UAC Interface not found");
    UAC_RETURN_ON_FALSE(uac_alt_param, ESP_ERR_INVALID_ARG, "Wrong argument");
    UAC_RETURN_ON_FALSE(iface_alt > 0, ESP_ERR_INVALID_ARG, "Invalid alt setting");
    UAC_RETURN_ON_FALSE(iface_alt <= iface->dev_info.iface_alt_num, ESP_ERR_INVALID_ARG, "Invalid alt setting");
    memcpy(uac_alt_param, &iface->iface_alt[iface_alt - 1].dev_alt_param, sizeof(uac_host_dev_alt_param_t));
    return ESP_OK;
}

esp_err_t uac_host_printf_device_param(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_FALSE(iface, ESP_ERR_INVALID_STATE, "UAC Interface not found");
    uac_host_dev_info_t dev_info;
    UAC_RETURN_ON_ERROR(uac_host_get_device_info(uac_dev_handle, &dev_info), "Unable to get UAC device params");
    printf("find UAC 1.0 %s device\n", dev_info.type == UAC_STREAM_TX ? "Speaker" : "Microphone");
    printf("interface number: %d\n", dev_info.iface_num);
    printf("total alt interfaces number: %d\n", dev_info.iface_alt_num);

    for (int i = 1; i <= dev_info.iface_alt_num; i++) {
        uac_host_dev_alt_param_t iface_alt_params;
        ESP_ERROR_CHECK(uac_host_get_device_alt_param(uac_dev_handle, i, &iface_alt_params));
        printf("--------alt interface[%d]--------- \nchannels = %d \nbit_resolution = %d \nsample_freq: \n",
               i, iface_alt_params.channels, iface_alt_params.bit_resolution);
        if (iface_alt_params.sample_freq_type) {
            for (int j = 0; j < iface_alt_params.sample_freq_type; j++) {
                printf("\t%" PRIu32 "\n", iface_alt_params.sample_freq[j]);
            }
        } else {
            printf(">= \t%" PRIu32 "\n", iface_alt_params.sample_freq_lower);
            printf("<= \t%" PRIu32 "\n", iface_alt_params.sample_freq_upper);
        }
    }
    return ESP_OK;
}

// ------------------------ USB UAC Host driver API ----------------------------

esp_err_t uac_host_device_start(uac_host_device_handle_t uac_dev_handle, const uac_host_stream_config_t *stream_config)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);

    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(iface->parent);
    UAC_RETURN_ON_FALSE(stream_config->bit_resolution, ESP_ERR_INVALID_ARG, "Invalid bit resolution");
    UAC_RETURN_ON_FALSE(stream_config->channels, ESP_ERR_INVALID_ARG, "Invalid number of channels");
    UAC_RETURN_ON_FALSE(stream_config->sample_freq, ESP_ERR_INVALID_ARG, "Invalid sample frequency");

    // get the mutex first to change the device/interface state
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    if (UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state) {
        uac_host_interface_unlock(iface);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    bool iface_claimed = false;
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_IDLE == iface->state), ESP_ERR_INVALID_STATE, "Interface wrong state");

    // check if any alt setting meets the channels, sample frequency and bit resolution requirements
    // if not exist, return error. If exist, claim the interface and prepare transfer
    iface->cur_alt = UINT8_MAX;
    for (int i = 0; i < iface->dev_info.iface_alt_num; i++) {
        if (iface->iface_alt[i].dev_alt_param.channels == stream_config->channels &&
                iface->iface_alt[i].dev_alt_param.bit_resolution == stream_config->bit_resolution) {
            // check if the sample frequency is in the list or in the range
            if (iface->iface_alt[i].dev_alt_param.sample_freq_type > 0) {
                for (int j = 0; j < iface->iface_alt[i].dev_alt_param.sample_freq_type; j++) {
                    if (iface->iface_alt[i].dev_alt_param.sample_freq[j] == stream_config->sample_freq) {
                        iface->cur_alt = i;
                        iface->iface_alt[i].cur_sampling_freq = stream_config->sample_freq;
                        break;
                    }
                }
            } else if (iface->iface_alt[i].dev_alt_param.sample_freq_lower <= stream_config->sample_freq &&
                       iface->iface_alt[i].dev_alt_param.sample_freq_upper >= stream_config->sample_freq) {
                iface->cur_alt = i;
                iface->iface_alt[i].cur_sampling_freq = stream_config->sample_freq;
                break;
            }
        }
    }

    UAC_GOTO_ON_FALSE(iface->cur_alt != UINT8_MAX, ESP_ERR_NOT_FOUND, "No suitable alt setting found");

    // enqueue multiple transfers to make sure the data is not lost
    iface->xfer_num = CONFIG_UAC_NUM_ISOC_URBS;
    iface->packet_num = CONFIG_UAC_NUM_PACKETS_PER_URB;
    iface->packet_size = iface->iface_alt[iface->cur_alt].cur_sampling_freq * stream_config->channels * stream_config->bit_resolution / 8 / 1000;
    iface->flags |= stream_config->flags;
    // if the packet size is not an integer, we need to add one more byte
    if (iface->iface_alt[iface->cur_alt].cur_sampling_freq * stream_config->channels * stream_config->bit_resolution / 8 % 1000) {
        ESP_LOGD(TAG, "packet_size %" PRIu32 " is not an integer, add one more byte", iface->packet_size);
        iface->packet_size++;
    }
    assert(iface->packet_size <= iface->iface_alt[iface->cur_alt].ep_mps);

    // Claim Interface and prepare transfer
    UAC_GOTO_ON_ERROR(uac_host_interface_claim_and_prepare_transfer(iface), "Unable to claim Interface");
    iface_claimed = true;

    if (!(iface->flags & FLAG_STREAM_SUSPEND_AFTER_START)) {
        UAC_GOTO_ON_ERROR(uac_host_interface_resume(iface), "Unable to enable UAC Interface");
    }
    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    if (iface_claimed) {
        uac_host_interface_release_and_free_transfer(iface);
    }
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_suspend(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);

    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    if (UAC_INTERFACE_STATE_READY == iface->state) {
        uac_host_interface_unlock(iface);
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state), ESP_ERR_INVALID_STATE, "device not active");
    UAC_GOTO_ON_ERROR(uac_host_interface_suspend(iface), "Unable to disable UAC Interface");

    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_resume(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);

    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    if (UAC_INTERFACE_STATE_ACTIVE == iface->state) {
        uac_host_interface_unlock(iface);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_READY == iface->state), ESP_ERR_INVALID_STATE, "device not ready");
    UAC_GOTO_ON_ERROR(uac_host_interface_resume(iface), "Unable to enable UAC Interface");

    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_stop(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);

    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    if (UAC_INTERFACE_STATE_ACTIVE == iface->state) {
        UAC_GOTO_ON_ERROR(uac_host_interface_suspend(iface), "Unable to disable UAC Interface");
    }

    if (UAC_INTERFACE_STATE_READY == iface->state) {
        UAC_GOTO_ON_ERROR(uac_host_interface_release_and_free_transfer(iface), "Unable to release UAC Interface");
    }

    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_read(uac_host_device_handle_t uac_dev_handle, uint8_t *data, uint32_t size, uint32_t *bytes_read, uint32_t timeout)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(data);
    UAC_RETURN_ON_INVALID_ARG(bytes_read);

    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    if (UAC_INTERFACE_STATE_ACTIVE != iface->state) {
        uac_host_interface_unlock(iface);
        return ESP_ERR_INVALID_STATE;
    }
    uac_host_interface_unlock(iface);

    size_t data_len = _ring_buffer_get_len(iface->ringbuf);
    if (data_len > size) {
        data_len = size;
    }
    esp_err_t ret = _ring_buffer_pop(iface->ringbuf, data, data_len, (size_t *)bytes_read, timeout);

    if (ESP_OK != ret) {
        ESP_LOGD(TAG, "RX Ringbuffer read failed");
        return ret;
    }

    return ESP_OK;
}

esp_err_t uac_host_device_write(uac_host_device_handle_t uac_dev_handle, uint8_t *data, uint32_t size, uint32_t timeout)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(data);

    // Only guarantee the data is written to the ringbuffer, in the case
    // 1. the interface is active when the write is called
    // Data will not be sent to the device and dropped in any of the following cases:
    // 1. the pipe state changed to inactive during or after the ringbuffer write
    // 2. the pipe state changed to inactive during continuous transfer submit
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    if (UAC_INTERFACE_STATE_ACTIVE != iface->state) {
        uac_host_interface_unlock(iface);
        return ESP_ERR_INVALID_STATE;
    }
    uac_host_interface_unlock(iface);

    esp_err_t ret = _ring_buffer_push(iface->ringbuf, data, size, timeout);

    if (ESP_OK != ret) {
        ESP_LOGD(TAG, "TX Ringbuffer write failed");
        return ret;
    }

    // We need to submit the transfer if there is free transfer in the list
    for (int i = 0; i < iface->xfer_num; i++) {
        UAC_ENTER_CRITICAL();
        if (iface->free_xfer_list[i]) {
            size_t data_len = _ring_buffer_get_len(iface->ringbuf);
            if (data_len == 0) {
                goto exit_critical;
            }
            // if interface state changed to inactive during blocking write
            // we need to return invalid state to safely exit the write function
            if (UAC_INTERFACE_STATE_ACTIVE != iface->state) {
                ret = ESP_ERR_INVALID_STATE;
                goto exit_critical;
            }
            iface->xfer_list[i] = iface->free_xfer_list[i];
            iface->free_xfer_list[i] = NULL;
            iface->xfer_list[i]->status = USB_TRANSFER_STATUS_COMPLETED;
            UAC_EXIT_CRITICAL();
            stream_tx_xfer_submit(iface->xfer_list[i]);
            UAC_ENTER_CRITICAL();
        }
exit_critical:
        UAC_EXIT_CRITICAL();
    }

    return ret;
}

esp_err_t uac_host_get_device_info(uac_host_device_handle_t uac_dev_handle, uac_host_dev_info_t *uac_dev_info)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_FALSE(iface, ESP_ERR_INVALID_STATE, "UAC Interface not found");
    UAC_RETURN_ON_FALSE(uac_dev_info, ESP_ERR_INVALID_ARG, "Wrong argument");
    memcpy(uac_dev_info, &iface->dev_info, sizeof(uac_host_dev_info_t));
    return ESP_OK;
}

esp_err_t uac_host_device_set_mute(uac_host_device_handle_t uac_dev_handle, bool mute)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    // Check if the device is active or ready
    UAC_RETURN_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                        ESP_ERR_INVALID_STATE, "device not ready or active");

    UAC_RETURN_ON_ERROR(uac_cs_request_set_mute(iface, mute), "Unable to set mute");
    return ESP_OK;
}

esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_FALSE(volume <= 100, ESP_ERR_INVALID_ARG, "Invalid volume value");
    // Check if the device is active or ready
    UAC_RETURN_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                        ESP_ERR_INVALID_STATE, "device not ready or active");

    uint32_t volume_db = volume * UAC_SPK_VOLUME_STEP + UAC_SPK_VOLUME_MIN;
    UAC_RETURN_ON_ERROR(uac_cs_request_set_volume(iface, volume_db), "Unable to set volume");
    return ESP_OK;
}

esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t uac_dev_handle, uint32_t volume_db)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    // Check if the device is active or ready
    UAC_RETURN_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                        ESP_ERR_INVALID_STATE, "device not ready or active");

    UAC_RETURN_ON_ERROR(uac_cs_request_set_volume(iface, volume_db), "Unable to set volume");
    return ESP_OK;
}
