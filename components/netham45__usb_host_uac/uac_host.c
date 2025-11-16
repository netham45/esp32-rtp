/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
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

#ifndef USB_FEATURE_ENDPOINT_HALT
#define USB_FEATURE_ENDPOINT_HALT 0x00
#endif

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
#define INTERFACE_FLAGS_OFFSET              (16)
#define FLAG_INTERFACE_WAIT_USER_DELETE     (1 << INTERFACE_FLAGS_OFFSET)
#define UAC_EP_DIR_IN                       (0x80)
#define VOLUME_DB_MIN                       (-127.9961f)
#define VOLUME_DB_MAX                       (127.9961f)
#define UAC_CTRL_RANGE_MAX_SUBRANGES        CONFIG_UAC_FREQ_NUM_MAX
#define UAC_CTRL_RANGE_SUBRANGE_SIZE_4      (sizeof(int32_t) * 2 + sizeof(uint32_t))
#define UAC_CTRL_RANGE_MAX_BUFFER           (sizeof(uint16_t) + UAC_CTRL_RANGE_MAX_SUBRANGES * UAC_CTRL_RANGE_SUBRANGE_SIZE_4)
#define UAC_FS_MAX_ISOC_MPS                 (600)

#if defined(USB_TRANSFER_STATUS_STALL)
#define UAC_TRANSFER_STATUS_STALL USB_TRANSFER_STATUS_STALL
#elif defined(USB_TRANSFER_STATUS_STALLED)
#define UAC_TRANSFER_STATUS_STALL USB_TRANSFER_STATUS_STALLED
#endif

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
    uint16_t spec_version;                          /*!< Reported UAC specification version */
    SemaphoreHandle_t device_busy;                  /*!< UAC device main mutex */
    SemaphoreHandle_t ctrl_xfer_done;               /*!< Control transfer semaphore */
    usb_transfer_t *ctrl_xfer;                      /*!< Pointer to control transfer buffer */
    uint8_t ctrl_iface_num;                         /*!< Control interface number */
    uint8_t *cs_ac_desc;                            /*!< Class-Specific Audio Control Interface descriptor */
    bool clock_ctrl_blacklisted;                    /*!< Device rejects clock selector/source controls */
} uac_device_t;

/**
 * @brief UAC Interface state
*/
typedef enum {
    UAC_INTERFACE_STATE_NOT_INITIALIZED = 0x00,     /*!< UAC Interface not initialized */
    UAC_INTERFACE_STATE_IDLE,                       /*!< UAC Interface has been opened but not started */
    UAC_INTERFACE_STATE_READY,                      /*!< UAC Interface has started but stream is suspended */
    UAC_INTERFACE_STATE_ACTIVE,                     /*!< UAC Interface is streaming */
    UAC_INTERFACE_STATE_SUSPENDING,                 /*!< UAC Interface is suspending */
} uac_iface_state_t;

/**
 * @brief UAC Interface alternate setting parameters
 */
typedef struct uac_interface_alt {
    // variable only change by app operation, protected by mutex
    uint32_t cur_sampling_freq;                /*!< current sampling frequency */
    // constant values after interface opening
    uint8_t alt_idx;                           /*!< audio stream alternate setting number */
    uint8_t ep_addr;                           /*!< audio stream endpoint number */
    uint16_t ep_mps;                           /*!< clamped endpoint max packet size */
    uint16_t ep_mps_raw;                       /*!< raw endpoint max packet size from descriptor */
    uint8_t ep_attr;                           /*!< audio stream endpoint attributes */
    uint8_t interval;                          /*!< audio stream endpoint interval */
    uint8_t feedback_ep_addr;                  /*!< optional feedback endpoint */
    bool feedback_ep_present;                  /*!< true if feedback endpoint descriptor found */
    uint8_t connected_terminal;                /*!< connected terminal ID */
    uint8_t feature_unit;                      /*!< connected feature unit ID */
    uint8_t vol_ch_map;                        /*!< volume channel map */
    uint8_t mute_ch_map;                       /*!< mute channel map */
    bool freq_ctrl_supported;                  /*!< sampling frequency control supported */
    uint8_t clock_source_id;                   /*!< clock source entity for UAC2 */
    uint8_t clock_selector_id;                 /*!< clock selector entity for UAC2 (if any) */
    uint32_t channel_config;                   /*!< channel map bitmap if provided */
    bool freq_info_valid;                      /*!< true when clock range info has been fetched */
    bool alt_reset_to_zero;                    /*!< true if suspend forced alt 0 */
    bool ep_supported;                         /*!< true if endpoint max packet size fits FS host */
    uac_host_dev_alt_param_t dev_alt_param;    /*!< audio stream alternate setting parameters */
} uac_iface_alt_t;

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
    uint8_t cur_vol;                           /*!< volume % 0-100 */
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
    int16_t vol_min_db;                        /*!< volume min with 1/256 db step */
    int16_t vol_max_db;                        /*!< volume max with 1/256 db step */
    int16_t vol_res_db;                        /*!< volume resolution with 1/256 db step */
    uac_iface_alt_t *iface_alt;                /*!< audio stream alternate setting */
    bool last_set_interface_success;           /*!< true if most recent SET_INTERFACE completed */
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
static uint16_t uac_device_get_ac_total_length(const uac_device_t *uac_device);
static inline uint8_t uac_iface_get_request_interface(const uac_iface_t *iface)
{
    if (iface && iface->parent) {
        return iface->parent->ctrl_iface_num;
    }
    return 0;
}
static uint8_t uac_device_resolve_clock_source(const uac_device_t *uac_device, uint8_t entity_id, uint8_t *selector_id);
static uint8_t uac_device_get_terminal_clock_source(const uac_device_t *uac_device, uint8_t terminal_id, uint8_t *selector_id);
static esp_err_t uac2_clock_set_sample_rate(uac_iface_t *iface, uint32_t freq);
static esp_err_t uac2_clock_read_sample_ranges(uac_iface_t *iface, uac_iface_alt_t *iface_alt);
static bool uac_iface_alt_freq_ready(const uac_iface_alt_t *iface_alt);
static void uac_iface_alt_apply_freq_defaults(uac_iface_alt_t *iface_alt, uint8_t prev_type,
                                              const uint32_t prev_freq[], uint32_t prev_lower,
                                              uint32_t prev_upper, bool disable_freq_ctrl);
static void uac_log_ac_entities(const uac_device_t *uac_device);
static esp_err_t uac_host_send_set_interface(uac_iface_t *iface, uint8_t alt_setting, bool tolerate_stall);
static esp_err_t uac_host_send_clear_feature(uac_device_t *device, uint8_t ep_addr);
static void uac_host_sync_reset_pipe(uac_device_t *device, uint8_t ep_addr);
static void uac_host_reset_stream_pipes(uac_iface_t *iface);

// --------------------------- Utility Functions --------------------------------
/**
 * @brief Convert volume U16 type to UAC volume db, for details, please refer to UAC 1.0 spec P.77
 * 0x7FFF: 127.9961db
 * 0x0100: 1.0000db
 * 0x0000: 0.0000db
 * 0xFF00: -1.0000db (there is a typo in UAC 1.0 spec, it should be 0xFF00)
 * 0xFE00: -2.0000db
 * 0x8001: -127.9961db
 * @param[in] volume      Volume in db
 * @return float
 */
static float _volume_db_i16_2_f(int16_t volume_db)
{
    return (float)(volume_db) / 256.0f;
}

static int16_t _volume_db_f_2_i16(float volume_db_f)
{
    if (volume_db_f > VOLUME_DB_MAX) {
        volume_db_f = VOLUME_DB_MAX;
    } else if (volume_db_f < VOLUME_DB_MIN) {
        volume_db_f = VOLUME_DB_MIN;
    }
    return (int16_t)(volume_db_f * 256);
}

static uint16_t uac_device_get_ac_total_length(const uac_device_t *uac_device)
{
    if (!uac_device || !uac_device->cs_ac_desc) {
        return 0;
    }
    if (uac_device->spec_version >= UAC_VERSION_2_0) {
        const uac2_ac_header_desc_t *header = (const uac2_ac_header_desc_t *)uac_device->cs_ac_desc;
        return header->wTotalLength;
    }
    const uac_ac_header_desc_t *header = (const uac_ac_header_desc_t *)uac_device->cs_ac_desc;
    return header->wTotalLength;
}

static const uac_desc_header_t *uac_device_find_desc_any(const uac_device_t *uac_device, uint8_t entity_id, uint8_t *subtype)
{
    if (!uac_device || !uac_device->cs_ac_desc) {
        return NULL;
    }

    uint16_t remaining = uac_device_get_ac_total_length(uac_device);
    const uint8_t *cursor = uac_device->cs_ac_desc;

    while (remaining >= sizeof(uac_desc_header_t)) {
        const uac_desc_header_t *cs_desc = (const uac_desc_header_t *)cursor;
        if (cs_desc->bLength == 0 || cs_desc->bLength > remaining) {
            break;
        }

        uint8_t desc_subtype = cs_desc->bDescriptorSubtype;
        uint8_t current_id = 0;

        switch (desc_subtype) {
        case UAC_AC_INPUT_TERMINAL:
        case UAC_AC_OUTPUT_TERMINAL:
        case UAC_AC_MIXER_UNIT:
        case UAC_AC_SELECTOR_UNIT:
        case UAC_AC_FEATURE_UNIT:
        case UAC_AC_PROCESSING_UNIT:
        case UAC_AC_EXTENSION_UNIT:
        case UAC_AC_CLOCK_SOURCE:
        case UAC_AC_CLOCK_SELECTOR:
        case UAC_AC_CLOCK_MULTIPLIER:
            current_id = cursor[3];
            break;
        default:
            break;
        }

        if (current_id == entity_id && current_id != 0) {
            if (subtype) {
                *subtype = desc_subtype;
            }
            ESP_LOGD(TAG, "Found entity %u subtype=0x%02X", entity_id, desc_subtype);
            return cs_desc;
        }

        cursor += cs_desc->bLength;
        remaining -= cs_desc->bLength;
    }

    ESP_LOGD(TAG, "Entity %u not found in AC descriptors", entity_id);
    return NULL;
}

static uint8_t uac_device_resolve_clock_source(const uac_device_t *uac_device, uint8_t entity_id, uint8_t *selector_id)
{
    if (!entity_id || !uac_device) {
        return 0;
    }

    uint8_t subtype = 0;
    const uac_desc_header_t *desc = uac_device_find_desc_any(uac_device, entity_id, &subtype);
    if (!desc) {
        return 0;
    }

    switch (subtype) {
    case UAC_AC_CLOCK_SOURCE:
        return entity_id;
    case UAC_AC_CLOCK_SELECTOR: {
        const uac2_clock_selector_desc_t *selector = (const uac2_clock_selector_desc_t *)desc;
        if (selector_id && *selector_id == 0) {
            *selector_id = entity_id;
        }
        if (selector->bNrInPins == 0) {
            return 0;
        }
        const uint8_t *pins = selector->baCSourceID;
        uint8_t next_id = pins[0];
        return uac_device_resolve_clock_source(uac_device, next_id, selector_id);
    }
    case UAC_AC_CLOCK_MULTIPLIER: {
        const uac2_clock_multiplier_desc_t *mult = (const uac2_clock_multiplier_desc_t *)desc;
        return uac_device_resolve_clock_source(uac_device, mult->bCSourceID, selector_id);
    }
    default:
        break;
    }

    return 0;
}

static uint8_t uac_device_get_terminal_clock_source(const uac_device_t *uac_device, uint8_t terminal_id, uint8_t *selector_id)
{
    if (!uac_device || uac_device->spec_version < UAC_VERSION_2_0 || terminal_id == 0) {
        return 0;
    }

    uint8_t subtype = 0;
    const uac_desc_header_t *term_desc = uac_device_find_desc_any(uac_device, terminal_id, &subtype);
    if (!term_desc) {
        return 0;
    }

    uint8_t clock_id = 0;
    if (subtype == UAC_AC_INPUT_TERMINAL) {
        const uac2_ac_input_terminal_desc_t *in = (const uac2_ac_input_terminal_desc_t *)term_desc;
        clock_id = in->bCSourceID;
    } else if (subtype == UAC_AC_OUTPUT_TERMINAL) {
        const uac2_ac_output_terminal_desc_t *out = (const uac2_ac_output_terminal_desc_t *)term_desc;
        clock_id = out->bCSourceID;
    }

    return uac_device_resolve_clock_source(uac_device, clock_id, selector_id);
}

static bool uac_iface_alt_freq_ready(const uac_iface_alt_t *iface_alt)
{
    if (!iface_alt) {
        return false;
    }
    if (!iface_alt->clock_source_id) {
        return (iface_alt->dev_alt_param.sample_freq_type > 0) ||
               (iface_alt->dev_alt_param.sample_freq_lower != 0) ||
               (iface_alt->dev_alt_param.sample_freq_upper != 0);
    }
    return iface_alt->freq_info_valid;
}

static void uac_iface_alt_apply_freq_defaults(uac_iface_alt_t *iface_alt, uint8_t prev_type,
                                              const uint32_t prev_freq[], uint32_t prev_lower,
                                              uint32_t prev_upper, bool disable_freq_ctrl)
{
    if (!iface_alt) {
        return;
    }
    if (prev_type > 0) {
        iface_alt->dev_alt_param.sample_freq_type = prev_type;
        memcpy(iface_alt->dev_alt_param.sample_freq, prev_freq, sizeof(uint32_t) * UAC_FREQ_NUM_MAX);
        // Do not touch lower/upper—they alias the first two entries via union
    } else if (prev_lower || prev_upper) {
        iface_alt->dev_alt_param.sample_freq_type = 0;
        iface_alt->dev_alt_param.sample_freq_lower = prev_lower;
        iface_alt->dev_alt_param.sample_freq_upper = prev_upper;
    } else {
        iface_alt->dev_alt_param.sample_freq_type = MIN(2, UAC_FREQ_NUM_MAX);
        if (UAC_FREQ_NUM_MAX > 0) {
            iface_alt->dev_alt_param.sample_freq[0] = 44100;
        }
        if (UAC_FREQ_NUM_MAX > 1) {
            iface_alt->dev_alt_param.sample_freq[1] = 48000;
        }
        for (int i = 2; i < UAC_FREQ_NUM_MAX; i++) {
            iface_alt->dev_alt_param.sample_freq[i] = 0;
        }
    }
    iface_alt->freq_info_valid = true;
    if (disable_freq_ctrl) {
        iface_alt->freq_ctrl_supported = false;
    }
}

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
        ESP_LOGI(TAG, "buffer is too small, push failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t _ring_buffer_pop(RingbufHandle_t ringbuf_hdl, uint8_t *buf, size_t req_bytes, size_t *read_bytes, TickType_t ticks_to_wait)
{
    assert(ringbuf_hdl && buf && read_bytes);
    /* Read up to req_bytes, blocking up to ticks_to_wait in total.
     * xRingbufferReceiveUpTo may return fewer bytes than requested even if
     * the caller asked for more, so loop until we've collected the full
     * amount or the timeout expires. If nothing is read before timeout,
     * return ESP_ERR_TIMEOUT. If some bytes were read and timeout expires, return
     * ESP_OK with the actual number of bytes read.
     */
    if (req_bytes == 0) {
        *read_bytes = 0;
        return ESP_OK;
    }

    size_t total_read = 0;
    TickType_t start_ticks = xTaskGetTickCount();

    while (total_read < req_bytes) {
        size_t chunk_read = 0;
        size_t want = req_bytes - total_read;

        /* compute remaining ticks to wait */
        TickType_t elapsed = xTaskGetTickCount() - start_ticks;
        TickType_t remain = 0;
        if (ticks_to_wait == portMAX_DELAY) {
            remain = portMAX_DELAY;
        } else if (elapsed >= ticks_to_wait) {
            remain = 0;
        } else {
            remain = ticks_to_wait - elapsed;
        }

        uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &chunk_read, remain, want);

        if (!buf_rcv) {
            /* nothing available within remaining timeout */
            break;
        }

        /* copy received chunk */
        memcpy(buf + total_read, buf_rcv, chunk_read);
        vRingbufferReturnItem(ringbuf_hdl, (void *)buf_rcv);
        total_read += chunk_read;
        if (chunk_read == 0) {
            /* defensive: in case the chunk is zero length */
            break;
        }
    }

    if (total_read == 0) {
        return ESP_ERR_TIMEOUT;
    }

    *read_bytes = total_read;
    return ESP_OK;
}

/**
 * @brief UAC Host driver event handler internal task
 *
 */
static void event_handler_task(void *arg)
{
    ESP_LOGI(TAG, "USB UAC handling start");
    while (uac_host_handle_events(portMAX_DELAY) == ESP_OK) {
    }
    ESP_LOGI(TAG, "USB UAC handling stop");
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
 * @brief Return UAC Device from USB device VID and PID
 * @param[in] vid  USB device VID
 * @param[in] pid  USB device PID
 * @return uac_device_t Pointer to UAC Device
 */
static uac_device_t *get_uac_device_by_vid_pid(uint16_t vid, uint16_t pid)
{
    uac_iface_t *interface = NULL;

    UAC_ENTER_CRITICAL();
    STAILQ_FOREACH(interface, &s_uac_driver->uac_ifaces_tailq, tailq_entry) {
        if (vid == interface->dev_info.VID && pid == interface->dev_info.PID) {
            UAC_EXIT_CRITICAL();
            return interface->parent;
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
 * @brief Get UAC Interface pointer by vid, pid and Interface number
 * @param[in] vid          USB device VID
 * @param[in] pid          USB device PID
 * @param[in] iface_num    Interface number
 * @return uac_iface_t     Pointer to UAC Interface configuration structure
 */
static uac_iface_t *get_interface_by_vid_pid(uint16_t vid, uint16_t pid, uint8_t iface_num)
{
    uac_iface_t *interface = NULL;

    UAC_ENTER_CRITICAL();
    STAILQ_FOREACH(interface, &s_uac_driver->uac_ifaces_tailq, tailq_entry) {
        if (vid == interface->dev_info.VID && pid == interface->dev_info.PID && iface_num == interface->dev_info.iface_num) {
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
            ESP_LOGI(TAG, "UAC Unknown Descriptor Subtype %d", uac_cs_desc->bDescriptorSubtype);
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
            ESP_LOGI(TAG, "UAC Unknown Descriptor Subtype %d", uac_cs_desc->bDescriptorSubtype);
            break;
        }
        uac_cs_desc = (uac_desc_header_t *)GET_NEXT_DESC(uac_cs_desc, total_length, uac_desc_offset);
    }
    return 0;
}

static uint8_t *_uac_host_device_find_feature_unit_desc(const uint8_t *header_desc, uint8_t terminal_id, bool if_input)
{
    if (!header_desc) {
        return NULL;
    }

    uint8_t unit_id = terminal_id;
    uint8_t *feature_unit_desc = NULL;
    if (if_input) {
        while (feature_unit_desc == NULL && unit_id != 0) {
            unit_id = _uac_next_linked_uint_id(header_desc, unit_id, (uint8_t **)&feature_unit_desc);
            ESP_LOGI(TAG, "Input Terminal next linked unit ID %d", unit_id);
        }
    } else {
        while (feature_unit_desc == NULL && unit_id != 0) {
            unit_id = _uac_last_linked_uint_id(header_desc, unit_id, (uint8_t **)&feature_unit_desc);
            ESP_LOGI(TAG, "Output Terminal last linked unit ID %d", unit_id);
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
    uac_iface->parent = uac_device;
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
    if (!iface_desc) {
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    uac_iface->dev_info.iface_num = iface_desc->bInterfaceNumber;
    iface_alt_desc = GET_NEXT_INTERFACE_DESC(iface_desc, total_length, iface_alt_offset);
    // For every alternate setting
    while (iface_alt_desc != NULL) {
        // Check if the alternate setting is for the same interface
        if (iface_alt_desc->bInterfaceNumber != iface_desc->bInterfaceNumber) {
            break;
        }
        ESP_LOGI(TAG, "Found UAC bInterfaceNumber= %d, bAlternateSetting= %d",
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
                    if (uac_device->spec_version >= UAC_VERSION_2_0) {
                        const uac2_as_general_desc_t *as_general_desc = (const uac2_as_general_desc_t *)uac_desc;
                        iface_alt->dev_alt_param.spec_version = UAC_VERSION_2_0;
                        iface_alt->dev_alt_param.format_type = as_general_desc->bFormatType;
                        iface_alt->dev_alt_param.format_flags = as_general_desc->bmFormats;
                        iface_alt->dev_alt_param.format = UAC_FORMAT_TYPE_I;
                        iface_alt->dev_alt_param.channels = as_general_desc->bNrChannels;
                        iface_alt->dev_alt_param.channel_config = as_general_desc->bmChannelConfig;
                        iface_alt->channel_config = as_general_desc->bmChannelConfig;
                        iface_alt->connected_terminal = as_general_desc->bTerminalLink;
                        iface_alt->clock_source_id = uac_device_get_terminal_clock_source(uac_device, iface_alt->connected_terminal, &iface_alt->clock_selector_id);
                    } else {
                        const uac_as_general_desc_t *as_general_desc = (const uac_as_general_desc_t *)uac_desc;
                        iface_alt->dev_alt_param.spec_version = UAC_VERSION_1_0;
                        iface_alt->dev_alt_param.format = as_general_desc->wFormatTag;
                        iface_alt->dev_alt_param.format_type = UAC_FORMAT_TYPE_I;
                        iface_alt->dev_alt_param.format_flags = as_general_desc->wFormatTag;
                        iface_alt->connected_terminal = as_general_desc->bTerminalLink;
                        iface_alt->clock_source_id = 0;
                        iface_alt->channel_config = 0;
                        iface_alt->dev_alt_param.channel_config = 0;
                    }
                } else if (uac_desc->bDescriptorSubtype == UAC_AS_FORMAT_TYPE) {
                    if (uac_device->spec_version >= UAC_VERSION_2_0) {
                        const uac2_as_type_I_format_desc_t *as_format_type_desc = (const uac2_as_type_I_format_desc_t *)uac_desc;
                        if (as_format_type_desc->bFormatType != UAC_FORMAT_TYPE_I) {
                            ESP_LOGE(TAG, "UAC2 Format Type %d", as_format_type_desc->bFormatType);
                            UAC_GOTO_ON_FALSE(0, ESP_ERR_NOT_SUPPORTED, "UAC Format Type not supported");
                        }
                        iface_alt->dev_alt_param.bit_resolution = as_format_type_desc->bBitResolution;
                        iface_alt->dev_alt_param.subslot_size = as_format_type_desc->bSubslotSize;
                        iface_alt->dev_alt_param.sample_freq_type = 0;
                    } else {
                        const uac_as_type_I_format_desc_t *as_format_type_desc = (const uac_as_type_I_format_desc_t *)uac_desc;
                        if (as_format_type_desc->bFormatType != UAC_FORMAT_TYPE_I) {
                            ESP_LOGE(TAG, "UAC Format Type %d", as_format_type_desc->bFormatType);
                            UAC_GOTO_ON_FALSE(0, ESP_ERR_NOT_SUPPORTED, "UAC Format Type not supported");
                        }
                        iface_alt->dev_alt_param.channels = as_format_type_desc->bNrChannels;
                        iface_alt->dev_alt_param.bit_resolution = as_format_type_desc->bBitResolution;
                        iface_alt->dev_alt_param.subslot_size = as_format_type_desc->bSubframeSize;
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
        ESP_LOGI(TAG, "UAC AS Format Type %d, Bit Resolution %d, Sample Freq Type %d", as_format_type_desc->bFormatType, as_format_type_desc->bBitResolution, as_format_type_desc->bSamFreqType);
    }
} else {
    ESP_LOGW(TAG, "UAC CS bDescriptorSubtype=%d not supported", uac_desc->bDescriptorSubtype);
}
break;
}
            case USB_B_DESCRIPTOR_TYPE_ENDPOINT: {
                const usb_ep_desc_t *ep_desc_local = (const usb_ep_desc_t *)cs_desc;
                bool endpoint_is_in = ep_desc_local->bEndpointAddress & UAC_EP_DIR_IN;
                bool stream_ep_is_in = iface_alt->ep_addr ? (iface_alt->ep_addr & UAC_EP_DIR_IN) : endpoint_is_in;
                if (!iface_alt->ep_addr) {
                    ep_desc = ep_desc_local;
                    uint8_t hs_trans = (ep_desc->wMaxPacketSize >> 11) & 0x3;
                    if (hs_trans) {
                        ESP_LOGW(TAG, "Endpoint 0x%02X advertises %d transactions/microframe, ignoring (FS host)",
                                 ep_desc->bEndpointAddress, hs_trans + 1);
        }
        iface_alt->ep_addr = ep_desc->bEndpointAddress;
        iface_alt->ep_mps_raw = USB_EP_DESC_GET_MPS(ep_desc);
        iface_alt->ep_supported = iface_alt->ep_mps_raw <= UAC_FS_MAX_ISOC_MPS;
        iface_alt->ep_mps = MIN(iface_alt->ep_mps_raw, (uint16_t)UAC_FS_MAX_ISOC_MPS);
        if (!iface_alt->ep_supported) {
            ESP_LOGW(TAG, "Endpoint 0x%02X MPS %u exceeds FS limit %u (alt %d)",
                     ep_desc->bEndpointAddress, iface_alt->ep_mps_raw, UAC_FS_MAX_ISOC_MPS, iface_alt->alt_idx);
        }
        iface_alt->ep_attr = ep_desc->bmAttributes;
        iface_alt->interval = ep_desc->bInterval;
        uac_iface->dev_info.type = endpoint_is_in ? UAC_STREAM_RX : UAC_STREAM_TX;
        uint8_t *feature_unit_raw = _uac_host_device_find_feature_unit_desc((uint8_t *)uac_device->cs_ac_desc,
                                                                            iface_alt->connected_terminal,
                                                                            !(ep_desc->bEndpointAddress & UAC_EP_DIR_IN));
    iface_alt->vol_ch_map = 0;
    iface_alt->mute_ch_map = 0;
    if (feature_unit_raw) {
        if (uac_device->spec_version >= UAC_VERSION_2_0) {
            const uac2_ac_feature_unit_desc_t *feature_unit_desc = (const uac2_ac_feature_unit_desc_t *)feature_unit_raw;
                        iface_alt->feature_unit = feature_unit_desc->bUnitID;
                        size_t slot_count = 0;
                        if (feature_unit_desc->bLength > 6) {
                            slot_count = (feature_unit_desc->bLength - 6) / 4;
                        }
                        const uint8_t *ctrl_ptr = (const uint8_t *)feature_unit_desc->bmaControls;
                        for (size_t i = 0; i < slot_count; i++) {
                            uint32_t ctrl = ctrl_ptr[0] | (ctrl_ptr[1] << 8) | (ctrl_ptr[2] << 16) | (ctrl_ptr[3] << 24);
                            if (((ctrl >> UAC2_FU_CTRL_VOLUME_POS) & UAC_CTRL_ACCESS_RW) != UAC_CTRL_ACCESS_NONE) {
                                iface_alt->vol_ch_map |= BIT(i);
                            }
                            if (((ctrl >> UAC2_FU_CTRL_MUTE_POS) & UAC_CTRL_ACCESS_RW) != UAC_CTRL_ACCESS_NONE) {
                                iface_alt->mute_ch_map |= BIT(i);
                            }
                            ctrl_ptr += 4;
                        }
                    } else {
                        const uac_ac_feature_unit_desc_t *feature_unit_desc = (const uac_ac_feature_unit_desc_t *)feature_unit_raw;
                        iface_alt->feature_unit = feature_unit_desc->bUnitID;
                        for (size_t i = 0; i < (feature_unit_desc->bLength - 7) / feature_unit_desc->bControlSize; i++) {
                            if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_VOLUME) {
                                iface_alt->vol_ch_map |= BIT(i);
                            }
                            if (feature_unit_desc->bmaControls[i * feature_unit_desc->bControlSize] & UAC_FU_CONTROL_POS_MUTE) {
                                iface_alt->mute_ch_map |= BIT(i);
                            }
            }
        }
        ESP_LOGI(TAG, "UAC %s Feature Unit ID %d, Volume Ch Map %02X, Mute Ch Map %02X",
                 uac_iface->dev_info.type == UAC_STREAM_RX ? "RX" : "TX", iface_alt->feature_unit,
                 iface_alt->vol_ch_map, iface_alt->mute_ch_map);
    }
    ESP_LOGI(TAG, "UAC Endpoint 0x%02X, Max Packet Size %d, Attributes 0x%02X, Interval %d", ep_desc->bEndpointAddress, ep_desc->wMaxPacketSize, ep_desc->bmAttributes, ep_desc->bInterval);
                } else if (!iface_alt->feedback_ep_present && (endpoint_is_in != stream_ep_is_in)) {
                    iface_alt->feedback_ep_addr = ep_desc_local->bEndpointAddress;
                    iface_alt->feedback_ep_present = true;
                    ESP_LOGI(TAG, "UAC Feedback Endpoint 0x%02X", iface_alt->feedback_ep_addr);
                }
                break;
}
            case UAC_CS_ENDPOINT: {
                // we has got enough information to fill the uac_iface_alt_t, so we can break
                const uac_as_cs_ep_desc_t *cs_ep_desc = (const uac_as_cs_ep_desc_t *)cs_desc;
                if (cs_ep_desc->bDescriptorSubtype == UAC_EP_GENERAL) {
                iface_alt->freq_ctrl_supported = cs_ep_desc->bmAttributes & UAC_SAMPLING_FREQ_CONTROL;
                parse_continue = false;
                ESP_LOGI(TAG, "UAC EP General, Attributes 0x%02X", cs_ep_desc->bmAttributes);
                ESP_LOGI(TAG, "UAC EP Frequency Control %d", iface_alt->freq_ctrl_supported);
            }
                break;
            }
            default:
                break;
            }
            cs_desc = GET_NEXT_DESC(cs_desc, total_length, cs_offset);
        }
        // Get next alternate setting
        if (uac_device->spec_version >= UAC_VERSION_2_0) {
            esp_err_t freq_ret = uac2_clock_read_sample_ranges(uac_iface, iface_alt);
            if (freq_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to read clock ranges for iface %d alt %d (err=0x%x)",
                         iface_desc->bInterfaceNumber, iface_alt->alt_idx, freq_ret);
            }
        }
        iface_alt_desc = GET_NEXT_INTERFACE_DESC(iface_alt_desc, total_length, iface_alt_offset);
    }
    uac_iface->state = UAC_INTERFACE_STATE_NOT_INITIALIZED;
    uac_iface->dev_info.addr = uac_device->addr;
    uac_iface->dev_info.iface_num = iface_desc->bInterfaceNumber;
    uac_iface->dev_info.iface_alt_num = iface_alt_idx;
    uac_iface->dev_info.spec_version = uac_device->spec_version;
    ESP_LOGI(TAG, "UAC Interface %d, found total alternate %d", iface_desc->bInterfaceNumber, iface_alt_idx);

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
#ifdef CONFIG_UAC_PRINTF_CONFIGURATION_DESCRIPTOR
        print_uac_descriptors(config_desc);
#endif // CONFIG_UAC_PRINTF_CONFIGURATION_DESCRIPTOR
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

    uint8_t alt_setting = iface->iface_alt[iface->cur_alt].alt_idx;
    ESP_LOGI(TAG, "Claim interface=%d alt_setting=%d cur_alt=%d state=%d",
             iface->dev_info.iface_num, alt_setting, iface->cur_alt, iface->state);
    UAC_RETURN_ON_ERROR(usb_host_interface_claim(s_uac_driver->client_handle, iface->parent->dev_hdl, iface->dev_info.iface_num,
                                                 alt_setting), "Unable to claim Interface");
    iface->iface_alt[iface->cur_alt].alt_reset_to_zero = true;
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
            ESP_LOGI(TAG, "RX Ringbuffer overflow");
        } else {
            // else push data to ringbuffer
            for (int i = 0; i < in_xfer->num_isoc_packets; i++) {
                if (in_xfer->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) {
                    // copy data to ringbuffer
                    ESP_LOGI(TAG, "Bad RX Isoc packet %d status %d", i, in_xfer->isoc_packet_desc[i].status);
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
        if (actual_num_bytes < data_len) {
            size_t deficit = data_len - actual_num_bytes;
            memset(out_xfer->data_buffer + actual_num_bytes, 0, deficit);
            ESP_LOGW(TAG, "USB audio TX underflow: needed %u bytes, got %u (zero padded %u)",
                     (unsigned)data_len, (unsigned)actual_num_bytes, (unsigned)deficit);
        }
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
    iface->state = UAC_INTERFACE_STATE_SUSPENDING;

    if (iface->iface_alt[iface->cur_alt].alt_reset_to_zero) {
        ESP_LOGI(TAG, "Interface %d already at alt0, skipping suspend", iface->dev_info.iface_num);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Suspending interface %d, switching to alt0", iface->dev_info.iface_num);

    // Set Interface alternate setting to 0
    return ESP_OK;
    usb_setup_packet_t usb_request;
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&usb_request, iface->dev_info.iface_num, 0);
    // memcpy, because of alignment warning
    uac_cs_request_t uac_request = {0};
    memcpy(&uac_request, &usb_request, sizeof(usb_setup_packet_t));
    esp_err_t ret = uac_cs_request_set(iface->parent, &uac_request);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set Interface %d-%d Failed", iface->dev_info.iface_num, 0);
        return ret;
    }
    ESP_LOGI(TAG, "Set Interface %d-%d", iface->dev_info.iface_num, 0);
    iface->iface_alt[iface->cur_alt].alt_reset_to_zero = true;

    uint8_t ep_addr = iface->iface_alt[iface->cur_alt].ep_addr;
    usb_host_endpoint_halt(iface->parent->dev_hdl, ep_addr);
    usb_host_endpoint_flush(iface->parent->dev_hdl, ep_addr);
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

    // If we previously suspended (alt 0), switch back to the streaming alt
    uint8_t alt_setting = iface->cur_alt + 1;
    bool need_set_interface = iface->iface_alt[iface->cur_alt].alt_reset_to_zero;
    ESP_LOGI(TAG, "Resume interface=%d cur_alt=%d alt_setting=%d need_set=%d state=%d",
             iface->dev_info.iface_num, iface->cur_alt, alt_setting, need_set_interface, iface->state);
    bool windows_preflight = false;
    if (need_set_interface && iface->cur_alt != UINT8_MAX) {
        ESP_RETURN_ON_ERROR(uac_host_send_set_interface(iface, alt_setting, true), TAG, "Unable to set Interface alternate");
        iface->iface_alt[iface->cur_alt].alt_reset_to_zero = false;
    }
    windows_preflight = (iface->parent->spec_version >= UAC_VERSION_2_0) &&
                        (iface->cur_alt != UINT8_MAX) &&
                        iface->last_set_interface_success;
    // Set sampling frequency control
    if (iface->parent->spec_version >= UAC_VERSION_2_0) {
        if (!iface->iface_alt[iface->cur_alt].freq_info_valid && iface->iface_alt[iface->cur_alt].clock_source_id) {
            esp_err_t range_ret = uac2_clock_read_sample_ranges(iface, &iface->iface_alt[iface->cur_alt]);
            if (range_ret != ESP_OK) {
                ESP_LOGW(TAG, "Unable to refresh clock range for iface %d alt %d after resume (err=0x%x)",
                         iface->dev_info.iface_num, alt_setting, range_ret);
            }
        }
        if (iface->iface_alt[iface->cur_alt].freq_ctrl_supported) {
            ESP_LOGI(TAG, "Set clock %d frequency %" PRIu32, iface->iface_alt[iface->cur_alt].clock_source_id,
                     iface->iface_alt[iface->cur_alt].cur_sampling_freq);
            esp_err_t set_ret = uac2_clock_set_sample_rate(iface, iface->iface_alt[iface->cur_alt].cur_sampling_freq);
            if (set_ret == ESP_ERR_NOT_SUPPORTED || set_ret == ESP_ERR_INVALID_RESPONSE) {
                ESP_LOGW(TAG, "Clock %d rejected SET_CUR (err=0x%x); keeping device rate",
                         iface->iface_alt[iface->cur_alt].clock_source_id, set_ret);
                iface->iface_alt[iface->cur_alt].freq_ctrl_supported = false;
            } else {
                UAC_RETURN_ON_ERROR(set_ret, "Unable to program clock frequency");
            }
        }
    if (windows_preflight) {
        for (int i = 0; i < 2; i++) {
            esp_err_t sync_ret = uac_host_send_set_interface(iface, alt_setting, true);
            if (sync_ret != ESP_OK) {
                ESP_LOGW(TAG, "Post-clock SET_INTERFACE #%d failed (err=0x%x)", i, sync_ret);
                break;
            }
        }
    }
    } else if (iface->iface_alt[iface->cur_alt].freq_ctrl_supported) {
        ESP_LOGI(TAG, "Set EP %02X frequency %" PRIu32, iface->iface_alt[iface->cur_alt].ep_addr,
                 iface->iface_alt[iface->cur_alt].cur_sampling_freq);
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
            ESP_LOGI(TAG, "Found UAC Control, bInterfaceNumber=%d", iface_desc->bInterfaceNumber);
            uac_cs_desc = GET_NEXT_UAC_CS_DESC(iface_desc, total_length, uac_desc_offset);
            uac_device->ctrl_iface_num = iface_desc->bInterfaceNumber;
            // break if all UAC control class specific descriptors are parsed
            while (uac_cs_desc != NULL) {
                switch (uac_cs_desc->bDescriptorSubtype) {
                case UAC_AC_HEADER: {
                    const uint8_t *raw = (const uint8_t *)uac_cs_desc;
                    uint16_t spec_version = raw[3] | (raw[4] << 8);
                    if (spec_version != UAC_VERSION_1_0 && spec_version != UAC_VERSION_2_0) {
                        ESP_LOGW(TAG, "UAC version 0x%04X not supported", spec_version);
                        free(uac_device);
                        return ESP_ERR_NOT_SUPPORTED;
                    }
                    uac_device->spec_version = spec_version;
                    uint16_t cs_length = 0;
                    if (spec_version >= UAC_VERSION_2_0) {
                        const uac2_ac_header_desc_t *header_desc_v2 = (const uac2_ac_header_desc_t *)uac_cs_desc;
                        cs_length = header_desc_v2->wTotalLength;
                    } else {
                        const uac_ac_header_desc_t *header_desc_v1 = (const uac_ac_header_desc_t *)uac_cs_desc;
                        cs_length = header_desc_v1->wTotalLength;
                    }
                    uint8_t *cs_ac_desc = calloc(cs_length, sizeof(uint8_t));
                    UAC_GOTO_ON_FALSE(cs_ac_desc, ESP_ERR_NO_MEM, "Unable to allocate memory for UAC Control CS descriptor");
                    memcpy(cs_ac_desc, uac_cs_desc, cs_length);
                    uac_device->cs_ac_desc = cs_ac_desc;
                    ESP_LOGI(TAG, "UAC version 0x%04X detected", spec_version);
                    uac_log_ac_entities(uac_device);
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

    UAC_GOTO_ON_FALSE(uac_device->cs_ac_desc, ESP_ERR_NOT_FOUND, "UAC Control descriptors not found");
    if (uac_device->spec_version == 0) {
        uac_device->spec_version = UAC_VERSION_1_0;
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

    ESP_LOGI(TAG, "Remove addr %d device from list", uac_device->addr);

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
        ESP_LOGE(TAG, "Control Transfer Timeout");
        return ESP_ERR_TIMEOUT;
    }

    bool transfer_completed = (ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED);
#if defined(UAC_TRANSFER_STATUS_STALL)
    bool transfer_stalled = (ctrl_xfer->status == UAC_TRANSFER_STATUS_STALL);
#else
    bool transfer_stalled = false;
#endif
    if (transfer_stalled) {
        ESP_LOGW(TAG, "Control transfer stalled on EP0 (status=%d)", ctrl_xfer->status);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!transfer_completed) {
        ESP_LOGW(TAG, "Control transfer failed (status=%d)", ctrl_xfer->status);
        return ESP_ERR_INVALID_RESPONSE;
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
    ESP_LOGI(TAG, "SET req bmReqType=0x%02X bReq=0x%02X wValue=0x%04X wIndex=0x%04X wLen=%u",
             setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);

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
 * @brief UAC class specific request Get
 *
 * @param[in] uac_device     Pointer to UAC device structure
 * @param[in] req            Pointer to a class specific request structure
 * @param[out] actual_length Pointer to store the actual length of data received
 * @return esp_err_t
 */
static esp_err_t uac_cs_request_get(uac_device_t *uac_device, const uac_cs_request_t *req, size_t *actual_length)
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
    ESP_LOGI(TAG, "GET req bmReqType=0x%02X bReq=0x%02X wValue=0x%04X wIndex=0x%04X wLen=%u",
             setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);

    if (setup->bmRequestType == 0) {
        setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS
                               | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    }

    ret = uac_control_transfer(uac_device, USB_SETUP_PACKET_SIZE + setup->wLength, DEFAULT_CTRL_XFER_TIMEOUT_MS);

    if (ret == ESP_OK) {
        *actual_length = ctrl_xfer->actual_num_bytes - USB_SETUP_PACKET_SIZE;
        if (*actual_length <= req->wLength) {
            memcpy(req->data, ctrl_xfer->data_buffer + USB_SETUP_PACKET_SIZE, *actual_length);
        } else {
            ret = ESP_ERR_INVALID_SIZE;
        }
    } else {
        *actual_length = 0;
    }

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

static esp_err_t uac2_clock_read_sample_ranges(uac_iface_t *iface, uac_iface_alt_t *iface_alt)
{
    if (!iface || !iface_alt) {
        return ESP_ERR_INVALID_ARG;
    }

    iface_alt->freq_info_valid = false;
    uint8_t prev_type = iface_alt->dev_alt_param.sample_freq_type;
    uint32_t prev_freq[UAC_FREQ_NUM_MAX] = {0};
    memcpy(prev_freq, iface_alt->dev_alt_param.sample_freq, sizeof(prev_freq));
    uint32_t prev_lower = iface_alt->dev_alt_param.sample_freq_lower;
    uint32_t prev_upper = iface_alt->dev_alt_param.sample_freq_upper;

    if (!iface_alt->clock_source_id) {
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, true);
        return ESP_OK;
    }

    if (iface->parent && iface->parent->clock_ctrl_blacklisted) {
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, true);
        return ESP_OK;
    }

    uint8_t subtype = 0;
    const uac_desc_header_t *clock_desc_hdr = uac_device_find_desc_any(iface->parent, iface_alt->clock_source_id, &subtype);
    ESP_LOGD(TAG, "clock source lookup id=%u ptr=%p subtype=0x%02X", iface_alt->clock_source_id, clock_desc_hdr, subtype);
    if (!clock_desc_hdr || subtype != UAC_AC_CLOCK_SOURCE) {
        ESP_LOGW(TAG, "Clock source %d descriptor missing", iface_alt->clock_source_id);
        iface_alt->clock_source_id = 0;
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, true);
        return ESP_OK;
    }

    const uac2_clock_source_desc_t *clock_desc = (const uac2_clock_source_desc_t *)clock_desc_hdr;
    uint8_t ctrl_bits = (clock_desc->bmControls >> UAC_CLOCK_SOURCE_CTRL_SAM_FREQ_POS) & UAC_CTRL_ACCESS_RW;
    bool ctrl_present = (ctrl_bits != UAC_CTRL_ACCESS_NONE);
    iface_alt->freq_ctrl_supported = (ctrl_bits == UAC_CTRL_ACCESS_RW);

    if (!ctrl_present) {
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, true);
        return ESP_OK;
    }

    if (iface_alt->clock_selector_id) {
        uint8_t selector_cur = 0;
        uac_cs_request_t sel_req = {
            .bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
            .bRequest = UAC2_CS_REQ_CUR,
            .wValue = (UAC2_CLOCK_SELECTOR_CONTROL << 8),
            .wIndex = (iface_alt->clock_selector_id << 8) | uac_iface_get_request_interface(iface),
            .wLength = 1,
            .data = &selector_cur,
        };
        size_t sel_len = 0;
        esp_err_t sel_ret = uac_cs_request_get(iface->parent, &sel_req, &sel_len);
        if (sel_ret != ESP_OK) {
            ESP_LOGW(TAG, "Clock selector %d GET_CUR failed (err=0x%x)", iface_alt->clock_selector_id, sel_ret);
            if (sel_ret == ESP_ERR_NOT_SUPPORTED || sel_ret == ESP_ERR_INVALID_RESPONSE) {
                if (iface->parent) {
                    iface->parent->clock_ctrl_blacklisted = true;
                }
                uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, true);
                return ESP_OK;
            }
            return sel_ret;
        } else {
            ESP_LOGD(TAG, "Clock selector %d current source %u", iface_alt->clock_selector_id, selector_cur);
        }
    }

    uint8_t range_buf[UAC_CTRL_RANGE_MAX_BUFFER] = {0};
    uac_cs_request_t req = {
        .bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        .bRequest = UAC2_CS_REQ_RANGE,
        .wValue = (UAC2_CLOCK_CTRL_SAM_FREQ << 8),
        .wIndex = (iface_alt->clock_source_id << 8) | uac_iface_get_request_interface(iface),
        .wLength = sizeof(range_buf),
        .data = range_buf,
    };

    size_t actual_length = 0;
    esp_err_t ret = uac_cs_request_get(iface->parent, &req, &actual_length);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "Clock %d does not support GET_RANGE (err=0x%x); using descriptor defaults",
                     iface_alt->clock_source_id, ret);
        } else {
            ESP_LOGW(TAG, "Clock %d GET_RANGE failed (err=0x%x)", iface_alt->clock_source_id, ret);
            return ret;
        }
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, false);
        return ESP_OK;
    }

    if (actual_length < sizeof(uint16_t)) {
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, false);
        return ESP_OK;
    }

    uint16_t subrange_count = range_buf[0] | (range_buf[1] << 8);
    const uint8_t *cursor = range_buf + sizeof(uint16_t);
    size_t remaining = actual_length - sizeof(uint16_t);

    uint8_t discrete_count = 0;
    bool range_written = false;
    iface_alt->dev_alt_param.sample_freq_type = 0;
    iface_alt->dev_alt_param.sample_freq_lower = 0;
    iface_alt->dev_alt_param.sample_freq_upper = 0;

    for (uint16_t i = 0; i < subrange_count && remaining >= UAC_CTRL_RANGE_SUBRANGE_SIZE_4; i++) {
        int32_t min = (int32_t)(cursor[0] | (cursor[1] << 8) | (cursor[2] << 16) | (cursor[3] << 24));
        int32_t max = (int32_t)(cursor[4] | (cursor[5] << 8) | (cursor[6] << 16) | (cursor[7] << 24));
        uint32_t res = (uint32_t)(cursor[8] | (cursor[9] << 8) | (cursor[10] << 16) | (cursor[11] << 24));
        cursor += UAC_CTRL_RANGE_SUBRANGE_SIZE_4;
        remaining -= UAC_CTRL_RANGE_SUBRANGE_SIZE_4;

        if (min == max || res == 0) {
            uint32_t freq = (uint32_t)min;
            if (discrete_count < UAC_FREQ_NUM_MAX) {
                iface_alt->dev_alt_param.sample_freq[discrete_count++] = freq;
            } else {
                ESP_LOGW(TAG, "Too many discrete sample rates, dropping %" PRIi32, min);
            }
            continue;
        }

        if (res != 0 && min < max) {
            for (int32_t freq = min; freq <= max && discrete_count < UAC_FREQ_NUM_MAX; freq += (int32_t)res) {
                iface_alt->dev_alt_param.sample_freq[discrete_count++] = freq;
            }
            if ((min + (int32_t)res) <= max && discrete_count >= UAC_FREQ_NUM_MAX) {
                ESP_LOGW(TAG, "Sample rate set truncated due to CONFIG_UAC_FREQ_NUM_MAX");
            }
            continue;
        }

        if (!range_written) {
            iface_alt->dev_alt_param.sample_freq_lower = min;
            iface_alt->dev_alt_param.sample_freq_upper = max;
            range_written = true;
        }
    }

    if (discrete_count) {
        iface_alt->dev_alt_param.sample_freq_type = discrete_count;
        iface_alt->freq_info_valid = true;
    } else if (range_written) {
        iface_alt->dev_alt_param.sample_freq_type = 0;
        iface_alt->freq_info_valid = true;
    } else {
        uac_iface_alt_apply_freq_defaults(iface_alt, prev_type, prev_freq, prev_lower, prev_upper, false);
        ESP_LOGW(TAG, "Clock %d did not report any sample-rate subranges", iface_alt->clock_source_id);
    }

    return ESP_OK;
}

static esp_err_t uac2_clock_set_sample_rate(uac_iface_t *iface, uint32_t freq)
{
    if (!iface || iface->parent->spec_version < UAC_VERSION_2_0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uac_iface_alt_t *iface_alt = &iface->iface_alt[iface->cur_alt];
    if (!iface_alt->clock_source_id) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (iface->parent->clock_ctrl_blacklisted) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!iface_alt->freq_ctrl_supported) {
        ESP_LOGW(TAG, "Clock %d does not expose frequency control", iface_alt->clock_source_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t payload[4];
    payload[0] = freq & 0xff;
    payload[1] = (freq >> 8) & 0xff;
    payload[2] = (freq >> 16) & 0xff;
    payload[3] = (freq >> 24) & 0xff;

    uac_cs_request_t req = {
        .bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        .bRequest = UAC2_CS_REQ_CUR,
        .wValue = (UAC2_CLOCK_CTRL_SAM_FREQ << 8),
        .wIndex = (iface_alt->clock_source_id << 8) | uac_iface_get_request_interface(iface),
        .wLength = sizeof(payload),
        .data = payload,
    };

    esp_err_t ret = uac_cs_request_set(iface->parent, &req);
    if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGW(TAG, "Clock %d rejected SET_CUR (err=0x%x); disabling clock controls",
                 iface_alt->clock_source_id, ret);
        iface->parent->clock_ctrl_blacklisted = true;
        iface_alt->freq_ctrl_supported = false;
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ret;
}

/**
 * @brief UAC class specific request - Set Volume
 * @param[in] iface       Pointer to UAC interface structure
 * @param[in] volume_db      Volume to set, db
 * @return esp_err_t
 */
static esp_err_t uac_cs_request_set_volume(uac_iface_t *iface, int16_t volume_db)
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

    tmp[0] = volume_db & 0xff;
    tmp[1] = (volume_db >> 8) & 0xff;

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
    ESP_LOGI(TAG, "Set volume %.4fdb (0x%04X)", _volume_db_i16_2_f(volume_db), volume_db);
    return ret;
}

static esp_err_t uac_cs_request_get_volume(uac_iface_t *iface, int16_t *volume_db)
{
    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;
    uint8_t vol_ch_map = iface->iface_alt[iface->cur_alt].vol_ch_map;
    UAC_RETURN_ON_FALSE(feature_unit && vol_ch_map, ESP_ERR_NOT_SUPPORTED, "volume control not supported");
    uint8_t ctrl_iface_num = iface->parent->ctrl_iface_num;
    uint8_t tmp[2] = { 0, 0 };
    esp_err_t ret = ESP_OK;

    uac_cs_request_t get_volume = {
        .bRequest = UAC_GET_CUR,
        .wIndex = (feature_unit << 8) | (ctrl_iface_num & 0xff),
        .wLength = 2,
        .data = tmp
    };

    // get volume for the first logical channel as we don't support separate volume control for each channel
    for (size_t i = 0; i < 8; i++) {
        if (vol_ch_map & (1 << i)) {
            get_volume.wValue = (UAC_VOLUME_CONTROL << 8) | i;
            break;
        }
    }

    size_t actual_length = 0;
    ret = uac_cs_request_get(iface->parent, &get_volume, &actual_length);
    if (ret != ESP_OK || actual_length != get_volume.wLength) {
        ESP_LOGE(TAG, "Failed to get volume");
        return ESP_FAIL;
    }

    *volume_db = tmp[0] | (tmp[1] << 8);
    ESP_LOGI(TAG, "Get volume %.4fdb (0x%04X)", _volume_db_i16_2_f(*volume_db), *volume_db);
    return ret;
}

static esp_err_t uac_cs_request_get_volume_range(uac_iface_t *iface, int16_t *volume_min_db, int16_t *volume_max_db, int16_t *volume_res_db)
{
    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;
    uint8_t vol_ch_map = iface->iface_alt[iface->cur_alt].vol_ch_map;
    UAC_RETURN_ON_FALSE(feature_unit && vol_ch_map, ESP_ERR_NOT_SUPPORTED, "volume control not supported");
    uint8_t ctrl_iface_num = iface->parent->ctrl_iface_num;
    esp_err_t ret = ESP_OK;
    uint8_t tmp[2] = { 0, 0 };
    int16_t volume_min, volume_max, volume_res = 0;

    uac_cs_request_t get_volume = {
        .bRequest = UAC_GET_MIN,
        .wIndex = (feature_unit << 8) | (ctrl_iface_num & 0xff),
        .wLength = 2,
        .data = tmp
    };

    // get volume range from the first logical channel
    for (size_t i = 0; i < 8; i++) {
        if (vol_ch_map & (1 << i)) {
            get_volume.wValue = (UAC_VOLUME_CONTROL << 8) | i;
            break;
        }
    }

    size_t actual_length = 0;
    ret = uac_cs_request_get(iface->parent, &get_volume, &actual_length);
    if (ret != ESP_OK || actual_length != get_volume.wLength) {
        ESP_LOGE(TAG, "Failed to get volume min");
        return ESP_FAIL;
    }
    volume_min = tmp[0] | (tmp[1] << 8);

    get_volume.bRequest = UAC_GET_MAX;
    ret = uac_cs_request_get(iface->parent, &get_volume, &actual_length);
    if (ret != ESP_OK || actual_length != get_volume.wLength) {
        ESP_LOGE(TAG, "Failed to get volume max");
        return ESP_FAIL;
    }
    volume_max = tmp[0] | (tmp[1] << 8);

    get_volume.bRequest = UAC_GET_RES;
    ret = uac_cs_request_get(iface->parent, &get_volume, &actual_length);
    if (ret != ESP_OK || actual_length != get_volume.wLength) {
        ESP_LOGE(TAG, "Failed to get volume res");
        return ESP_FAIL;
    }
    volume_res = tmp[0] | (tmp[1] << 8);
    *volume_min_db = volume_min;
    *volume_max_db = volume_max;
    *volume_res_db = volume_res;

    ESP_LOGI(TAG, "Volume range: min %.4fdb (0x%04X), max %.4fdb (0x%04X), res %.4fdb (0x%04X)", _volume_db_i16_2_f(volume_min), volume_min,
             _volume_db_i16_2_f(volume_max), volume_max, _volume_db_i16_2_f(volume_res), volume_res);
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
    ESP_LOGI(TAG, "Set mute %d", mute);
    return ret;
}

static esp_err_t uac_cs_request_get_mute(uac_iface_t *iface, bool *mute)
{
    uint8_t feature_unit = iface->iface_alt[iface->cur_alt].feature_unit;
    uint8_t mute_ch_map = iface->iface_alt[iface->cur_alt].mute_ch_map;
    UAC_RETURN_ON_FALSE(feature_unit, ESP_ERR_NOT_SUPPORTED, "mute control not supported");
    uint8_t ctrl_iface_num = iface->parent->ctrl_iface_num;
    uint8_t tmp[1] = { 0 };
    esp_err_t ret = ESP_OK;

    uac_cs_request_t get_mute = {
        .bRequest = UAC_GET_CUR,
        .wIndex = (feature_unit << 8) | (ctrl_iface_num & 0xff),
        .wLength = 1,
        .data = tmp
    };

    // get mute for the first logical channel as we don't support separate mute control for each channel
    for (size_t i = 0; i < 8; i++) {
        if (mute_ch_map & (1 << i)) {
            get_mute.wValue = (UAC_MUTE_CONTROL << 8) | i;
            break;
        }
    }

    size_t actual_length = 0;
    ret = uac_cs_request_get(iface->parent, &get_mute, &actual_length);
    if (ret != ESP_OK || actual_length != get_mute.wLength) {
        ESP_LOGE(TAG, "Failed to get mute");
        return ESP_FAIL;
    }

    *mute = tmp[0];
    ESP_LOGI(TAG, "Get mute: %s", *mute ? "true" : "false");
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
    ESP_LOGI(TAG, "Install Succeed, Version: %d.%d.%d", USB_HOST_UAC_VER_MAJOR, USB_HOST_UAC_VER_MINOR, USB_HOST_UAC_VER_PATCH);
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

    ESP_LOGI(TAG, "Open Device addr %d, iface %d", config->addr, config->iface_num);
    // Check if the logic device/interface is already added
    uac_iface_t *uac_iface = get_interface_by_addr(config->addr, config->iface_num);
    if (uac_iface) {
        *uac_dev_handle = (uac_host_device_handle_t)uac_iface;
        ESP_LOGI(TAG, "Device addr %d, iface %d already opened", config->addr, config->iface_num);
        return ESP_OK;
    }

    // Check if the physical device is already added
    esp_err_t ret = ESP_OK;
    uac_device_t *uac_device = get_uac_device_by_addr(config->addr);
    bool new_device = false;
    usb_device_handle_t dev_hdl = NULL;
    if (!uac_device) {
        UAC_GOTO_ON_ERROR(usb_host_device_open(s_uac_driver->client_handle, config->addr, &dev_hdl), "Unable to open USB device");
        ESP_LOGI(TAG, "line %d, Open Device addr %d", __LINE__, config->addr);
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

    // Get the current volume range if the device supports volume control
    if (uac_iface->iface_alt[uac_iface->cur_alt].feature_unit && uac_iface->iface_alt[uac_iface->cur_alt].vol_ch_map) {
        ret = uac_cs_request_get_volume_range(uac_iface, &uac_iface->vol_min_db, &uac_iface->vol_max_db, &uac_iface->vol_res_db);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get volume range");
        }
    }

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

esp_err_t uac_host_device_open_with_vid_pid(uint16_t vid, uint16_t pid, const uac_host_device_config_t *config, uac_host_device_handle_t *uac_dev_handle)
{
    UAC_RETURN_ON_INVALID_ARG(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(config);
    UAC_RETURN_ON_FALSE(config->iface_num, ESP_ERR_INVALID_ARG, "Invalid interface number");
    UAC_RETURN_ON_FALSE(s_uac_driver, ESP_ERR_INVALID_STATE, "UAC Driver is not installed");
    UAC_RETURN_ON_FALSE(vid, ESP_ERR_INVALID_ARG, "Invalid vendor ID");
    UAC_RETURN_ON_FALSE(pid, ESP_ERR_INVALID_ARG, "Invalid product ID");

    // check if the device is already opened
    *uac_dev_handle = get_interface_by_vid_pid(vid, pid, config->iface_num);
    if (*uac_dev_handle) {
        ESP_LOGI(TAG, "Device VID 0x%04X, PID 0x%04X, Interface %d already opened", vid, pid, config->iface_num);
        return ESP_OK;
    }

    // check all connected devices
    int actual_count = 0;
    uint8_t dev_addr_list[CONFIG_UAC_DEV_ADDR_LIST_MAX] = {0};
    usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &actual_count);
    if (actual_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < actual_count; i++) {
        const usb_device_desc_t *dev_desc = NULL;
        usb_device_handle_t dev_hdl = NULL;
        uac_device_t *uac_dev = get_uac_device_by_vid_pid(vid, pid);
        uac_host_device_config_t config_copy = *config;
        config_copy.addr = dev_addr_list[i];
        if (uac_dev) {
            ESP_LOGI(TAG, "Found Device VID 0x%04X, PID 0x%04X", vid, pid);
            return uac_host_device_open(&config_copy, uac_dev_handle);
        } else {
            UAC_RETURN_ON_ERROR(usb_host_device_open(s_uac_driver->client_handle, dev_addr_list[i], &dev_hdl), "Unable to open USB device");
            UAC_RETURN_ON_ERROR(usb_host_get_device_descriptor(dev_hdl, &dev_desc), "Unable to get device descriptor");
            ESP_LOGI(TAG, "Found Device VID 0x%04X, PID 0x%04X", dev_desc->idVendor, dev_desc->idProduct);
            if (dev_desc->idVendor == vid && dev_desc->idProduct == pid) {
                usb_host_device_close(s_uac_driver->client_handle, dev_hdl);
                return uac_host_device_open(&config_copy, uac_dev_handle);
            }
            usb_host_device_close(s_uac_driver->client_handle, dev_hdl);
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t uac_host_device_close(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *uac_iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(uac_iface);

    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Close addr %d, iface %d, state %d", uac_iface->dev_info.addr, uac_iface->dev_info.iface_num, uac_iface->state);

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
        ESP_LOGI(TAG, "line %d, Close Device addr %d", __LINE__, uac_iface->parent->addr);
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
        // Unblock the low priority tasks waiting for the ringbuffer before deleting it
        vTaskDelay(pdMS_TO_TICKS(CONFIG_UAC_RINGBUF_SAFE_DELETE_WAITING_MS));
        vRingbufferDelete(uac_iface->ringbuf);
        uac_iface->ringbuf = NULL;
    }

    uac_iface->user_cb = NULL;
    uac_iface->user_cb_arg = NULL;
    ESP_LOGI(TAG, "User Remove addr %d, iface %d from list", uac_iface->dev_info.addr, uac_iface->dev_info.iface_num);
    uac_host_interface_delete(uac_iface);

    return ESP_OK;

fail:
    uac_host_interface_unlock(uac_iface);
    return ret;
}

esp_err_t uac_host_handle_events(TickType_t timeout)
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
    uac_iface_alt_t *alt = &iface->iface_alt[iface_alt - 1];
    if (iface->parent->spec_version >= UAC_VERSION_2_0 && !uac_iface_alt_freq_ready(alt) && alt->clock_source_id) {
        esp_err_t range_ret = uac2_clock_read_sample_ranges(iface, alt);
        if (range_ret != ESP_OK) {
            ESP_LOGW(TAG, "Unable to refresh clock range for iface %d alt %d when queried (err=0x%x)",
                     iface->dev_info.iface_num, iface_alt, range_ret);
        }
    }
    memcpy(uac_alt_param, &alt->dev_alt_param, sizeof(uac_host_dev_alt_param_t));
    return ESP_OK;
}

esp_err_t uac_host_printf_device_param(uac_host_device_handle_t uac_dev_handle)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_FALSE(iface, ESP_ERR_INVALID_STATE, "UAC Interface not found");
    uac_host_dev_info_t dev_info;
    UAC_RETURN_ON_ERROR(uac_host_get_device_info(uac_dev_handle, &dev_info), "Unable to get UAC device params");
    const char *spec_str = (dev_info.spec_version >= UAC_VERSION_2_0) ? "2.0" : "1.0";
    printf("find UAC %s %s device\n", spec_str, dev_info.type == UAC_STREAM_TX ? "Speaker" : "Microphone");
    printf("interface number: %d\n", dev_info.iface_num);
    printf("total alt interfaces number: %d\n", dev_info.iface_alt_num);

    for (int i = 1; i <= dev_info.iface_alt_num; i++) {
        uac_host_dev_alt_param_t iface_alt_params;
        ESP_ERROR_CHECK(uac_host_get_device_alt_param(uac_dev_handle, i, &iface_alt_params));
        printf("--------alt interface[%d]---------\n", i);
        printf("spec version: 0x%04X\n", iface_alt_params.spec_version);
        printf("channels = %d \nbit_resolution = %d \nsubslot_size = %d\n",
               iface_alt_params.channels, iface_alt_params.bit_resolution, iface_alt_params.subslot_size);
        if (iface_alt_params.format_flags) {
            printf("format_type = %d \nformat_flags = 0x%08" PRIx32 "\n",
                   iface_alt_params.format_type, iface_alt_params.format_flags);
        }
        if (iface_alt_params.channel_config) {
            printf("channel_config: 0x%08" PRIx32 "\n", iface_alt_params.channel_config);
        }
        printf("sample_freq: \n");
        if (iface_alt_params.sample_freq_type) {
            for (int j = 0; j < iface_alt_params.sample_freq_type; j++) {
                printf("\t%" PRIu32 "\n", iface_alt_params.sample_freq[j]);
            }
        } else if (iface_alt_params.sample_freq_lower || iface_alt_params.sample_freq_upper) {
            printf(">= \t%" PRIu32 "\n", iface_alt_params.sample_freq_lower);
            printf("<= \t%" PRIu32 "\n", iface_alt_params.sample_freq_upper);
        } else {
            printf("\t(not reported by device)\n");
        }
        bool supported = iface->iface_alt[i - 1].ep_supported;
        printf("endpoint MPS: raw=%u (clamped to %u)%s\n",
               iface->iface_alt[i - 1].ep_mps_raw,
               iface->iface_alt[i - 1].ep_mps,
               supported ? "" : " [exceeds FS limit]");
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

    // refresh clock ranges for UAC2 devices if not populated yet
    if (iface->parent->spec_version >= UAC_VERSION_2_0) {
        for (int i = 0; i < iface->dev_info.iface_alt_num; i++) {
            if (!uac_iface_alt_freq_ready(&iface->iface_alt[i]) && iface->iface_alt[i].clock_source_id) {
                esp_err_t range_ret = uac2_clock_read_sample_ranges(iface, &iface->iface_alt[i]);
                if (range_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Unable to refresh clock ranges for iface %d alt %d (err=0x%x)",
                             iface->dev_info.iface_num, i + 1, range_ret);
                }
            }
        }
    }

    // check if any alt setting meets the channels, sample frequency and bit resolution requirements
    // if not exist, return error. If exist, claim the interface and prepare transfer
    iface->cur_alt = UINT8_MAX;
    bool alt_rejected_due_to_mps = false;
    for (int i = 0; i < iface->dev_info.iface_alt_num; i++) {
        if (iface->iface_alt[i].dev_alt_param.channels == stream_config->channels &&
                iface->iface_alt[i].dev_alt_param.bit_resolution == stream_config->bit_resolution) {
            if (!iface->iface_alt[i].ep_supported) {
                ESP_LOGW(TAG, "Skip iface %d alt %d: endpoint MPS %u exceeds full-speed limit",
                         iface->dev_info.iface_num, i + 1, iface->iface_alt[i].ep_mps_raw);
                alt_rejected_due_to_mps = true;
                continue;
            }
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

    if (iface->cur_alt == UINT8_MAX) {
        if (alt_rejected_due_to_mps) {
            ESP_LOGE(TAG, "All matching alternate settings exceed the full-speed max packet limit (%u bytes)", UAC_FS_MAX_ISOC_MPS);
            ret = ESP_ERR_NOT_SUPPORTED;
            goto fail;
        }
        ret = ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "No suitable alternate setting found");
        goto fail;
    }

    // enqueue multiple transfers to make sure the data is not lost
    iface->xfer_num = CONFIG_UAC_NUM_ISOC_URBS;
    iface->packet_num = CONFIG_UAC_NUM_PACKETS_PER_URB;
    iface->packet_size = iface->iface_alt[iface->cur_alt].cur_sampling_freq * stream_config->channels * stream_config->bit_resolution / 8 / 1000;
    iface->flags |= stream_config->flags;
    // if the packet size is not an integer, we need to add one more byte
    if (iface->iface_alt[iface->cur_alt].cur_sampling_freq * stream_config->channels * stream_config->bit_resolution / 8 % 1000) {
        ESP_LOGI(TAG, "packet_size %" PRIu32 " is not an integer, add one more byte", iface->packet_size);
        iface->packet_size++;
    }
    if (iface->packet_size > iface->iface_alt[iface->cur_alt].ep_mps) {
        ESP_LOGW(TAG, "Computed packet_size %" PRIu32 " > EP MPS %u, trimming to fit", iface->packet_size,
                 iface->iface_alt[iface->cur_alt].ep_mps);
        iface->packet_size = iface->iface_alt[iface->cur_alt].ep_mps;
    }

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

    esp_err_t ret = _ring_buffer_pop(iface->ringbuf, data, size, (size_t *)bytes_read, timeout);

    if (ESP_OK != ret) {
        ESP_LOGI(TAG, "RX Ringbuffer read failed");
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
        ESP_LOGI(TAG, "TX Ringbuffer write failed");
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
    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    UAC_GOTO_ON_FALSE(UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state,
                      ESP_ERR_INVALID_STATE, "device not ready or active");

    UAC_GOTO_ON_ERROR(uac_cs_request_set_mute(iface, mute), "Unable to set mute");
    ESP_LOGI(TAG, "%s Interface %d-%d", mute ? "Mute" : "Unmute", iface->dev_info.iface_num, iface->cur_alt + 1);
    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_get_mute(uac_host_device_handle_t uac_dev_handle, bool *mute)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(mute);
    // Check if the device is active or ready
    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                      ESP_ERR_INVALID_STATE, "device not ready or active");

    UAC_GOTO_ON_ERROR(uac_cs_request_get_mute(iface, mute), "Unable to get mute");
    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_FALSE(volume <= 100, ESP_ERR_INVALID_ARG, "Invalid volume value");
    // Check if the device is active or ready
    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                      ESP_ERR_INVALID_STATE, "device not ready or active");

    // Calculate target volume in float to avoid the int16_t calculation overflow
    float volume_db_f = _volume_db_i16_2_f(iface->vol_min_db) + (_volume_db_i16_2_f(iface->vol_max_db) - _volume_db_i16_2_f(iface->vol_min_db)) * (float)volume / 100.0f;
    // Round to the nearest float value based the vol_res_db
    volume_db_f = roundf(volume_db_f / _volume_db_i16_2_f(iface->vol_res_db)) * _volume_db_i16_2_f(iface->vol_res_db);
    // Convert back to 16-bit value
    const int16_t volume_db = _volume_db_f_2_i16(volume_db_f);

    UAC_GOTO_ON_ERROR(uac_cs_request_set_volume(iface, volume_db), "Unable to set volume");
    // Backup the volume value for the get volume function
    iface->cur_vol = volume;
    ESP_LOGI(TAG, "Set volume %d%%, Interface %d-%d", volume, iface->dev_info.iface_num, iface->cur_alt + 1);
    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_get_volume(uac_host_device_handle_t uac_dev_handle, uint8_t *volume)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(volume);
    // Check if the device is active or ready
    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                      ESP_ERR_INVALID_STATE, "device not ready or active");

    // Return the backup volume value to avoid the volume reads differently than expected.
    // Because the device volume adjustment step may be relatively large,
    // For example, when the user sets 83%, the actual volume may be 80.
    if (iface->cur_vol) {
        *volume = iface->cur_vol;
        uac_host_interface_unlock(iface);
        return ESP_OK;
    }

    // Otherwise, get the volume from the device
    // Get volume range, calculate in dB float
    int16_t volume_db = 0;
    UAC_GOTO_ON_ERROR(uac_cs_request_get_volume(iface, &volume_db), "Unable to get volume");
    const float volume_db_f = _volume_db_i16_2_f(volume_db);
    // Calculate volume in percentage
    *volume = (uint8_t)roundf((volume_db_f - _volume_db_i16_2_f(iface->vol_min_db)) * 100.0f / (_volume_db_i16_2_f(iface->vol_max_db) - _volume_db_i16_2_f(iface->vol_min_db)));

    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t uac_dev_handle, int16_t volume_db)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    // Check if the device is active or ready
    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                      ESP_ERR_INVALID_STATE, "device not ready or active");
    // Check if the volume is within the range
    UAC_GOTO_ON_FALSE((volume_db >= iface->vol_min_db && volume_db <= iface->vol_max_db), ESP_ERR_INVALID_ARG, "Invalid volume value");
    UAC_GOTO_ON_ERROR(uac_cs_request_set_volume(iface, volume_db), "Unable to set volume");
    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}

esp_err_t uac_host_device_get_volume_db(uac_host_device_handle_t uac_dev_handle, int16_t *volume_db)
{
    uac_iface_t *iface = get_iface_by_handle(uac_dev_handle);
    UAC_RETURN_ON_INVALID_ARG(iface);
    UAC_RETURN_ON_INVALID_ARG(volume_db);
    // Check if the device is active or ready
    esp_err_t ret = ESP_OK;
    UAC_RETURN_ON_ERROR(uac_host_interface_try_lock(iface, DEFAULT_CTRL_XFER_TIMEOUT_MS), "Unable to lock UAC Interface");
    UAC_GOTO_ON_FALSE((UAC_INTERFACE_STATE_ACTIVE == iface->state || UAC_INTERFACE_STATE_READY == iface->state),
                      ESP_ERR_INVALID_STATE, "device not ready or active");

    UAC_GOTO_ON_ERROR(uac_cs_request_get_volume(iface, volume_db), "Unable to get volume");
    uac_host_interface_unlock(iface);
    return ESP_OK;

fail:
    uac_host_interface_unlock(iface);
    return ret;
}
static void uac_log_ac_entities(const uac_device_t *uac_device)
{
    if (!uac_device || !uac_device->cs_ac_desc) {
        return;
    }
    uint16_t total_length = uac_device_get_ac_total_length(uac_device);
    int offset = 0;
    const uac_desc_header_t *cs_desc = (const uac_desc_header_t *)uac_device->cs_ac_desc;
    ESP_LOGI(TAG, "Dumping AC descriptors (total_length=%u)", total_length);
    while (cs_desc && cs_desc->bLength && offset < total_length) {
        uint8_t subtype = cs_desc->bDescriptorSubtype;
        uint8_t entity_id = 0;
        switch (subtype) {
        case UAC_AC_INPUT_TERMINAL:
        case UAC_AC_OUTPUT_TERMINAL:
        case UAC_AC_MIXER_UNIT:
        case UAC_AC_SELECTOR_UNIT:
        case UAC_AC_FEATURE_UNIT:
        case UAC_AC_PROCESSING_UNIT:
        case UAC_AC_EXTENSION_UNIT:
        case UAC_AC_CLOCK_SOURCE:
        case UAC_AC_CLOCK_SELECTOR:
        case UAC_AC_CLOCK_MULTIPLIER:
            entity_id = *((const uint8_t *)cs_desc + 3);
            break;
        default:
            break;
        }
        ESP_LOGI(TAG, "  offset=%d len=%u subtype=0x%02X entity_id=%u", offset, cs_desc->bLength, subtype, entity_id);
        cs_desc = (const uac_desc_header_t *)GET_NEXT_DESC(cs_desc, total_length, offset);
    }
}
static esp_err_t uac_host_send_set_interface(uac_iface_t *iface, uint8_t alt_setting, bool tolerate_stall)
{
    if (!iface || iface->cur_alt == UINT8_MAX) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_setup_packet_t usb_request;
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&usb_request, iface->dev_info.iface_num, alt_setting);
    uac_cs_request_t uac_request = {0};
    memcpy(&uac_request, &usb_request, sizeof(usb_setup_packet_t));

    const int max_attempts = 2;
    esp_err_t set_ret = ESP_FAIL;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        set_ret = uac_cs_request_set(iface->parent, &uac_request);
        if (set_ret == ESP_OK) {
            ESP_LOGI(TAG, "Set Interface %d-%d", iface->dev_info.iface_num, alt_setting);
            iface->last_set_interface_success = true;
            return ESP_OK;
        }

        if (set_ret != ESP_ERR_NOT_SUPPORTED && set_ret != ESP_ERR_INVALID_RESPONSE) {
            break;
        }

        ESP_LOGW(TAG, "SET_INTERFACE alt %d stalled (attempt %d, err=0x%x)", alt_setting, attempt + 1, set_ret);
        uac_host_send_clear_feature(iface->parent, 0);
    }

    if (set_ret == ESP_ERR_NOT_SUPPORTED || set_ret == ESP_ERR_INVALID_RESPONSE || set_ret == ESP_ERR_TIMEOUT) {
        if (tolerate_stall) {
            ESP_LOGW(TAG, "SET_INTERFACE alt %d still stalled after retries; tolerating", alt_setting);
            uac_host_send_clear_feature(iface->parent, 0);
            iface->last_set_interface_success = false;
            return ESP_OK;
        }
    }

    iface->last_set_interface_success = false;
    return set_ret;
}

static esp_err_t uac_host_send_clear_feature(uac_device_t *device, uint8_t ep_addr)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    uac_cs_request_t clear_req = {
        .bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT,
        .bRequest = USB_B_REQUEST_CLEAR_FEATURE,
        .wValue = USB_FEATURE_ENDPOINT_HALT,
        .wIndex = ep_addr,
        .wLength = 0,
        .data = NULL,
    };
    esp_err_t ret = uac_cs_request_set(device, &clear_req);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CLEAR_FEATURE EP 0x%02X failed (0x%x)", ep_addr, ret);
    }
    return ret;
}

static void uac_host_sync_reset_pipe(uac_device_t *device, uint8_t ep_addr)
{
    if (!device || ep_addr == 0) {
        return;
    }
    ESP_LOGI(TAG, "Sync-reset pipe EP 0x%02X", ep_addr);
    uac_host_send_clear_feature(device, ep_addr);
}

static void uac_host_reset_stream_pipes(uac_iface_t *iface)
{
    if (!iface || iface->cur_alt == UINT8_MAX) {
        return;
    }
    uac_iface_alt_t *alt = &iface->iface_alt[iface->cur_alt];
    uac_host_sync_reset_pipe(iface->parent, alt->ep_addr);
    if (alt->feedback_ep_present) {
        uac_host_sync_reset_pipe(iface->parent, alt->feedback_ep_addr);
    }
}
