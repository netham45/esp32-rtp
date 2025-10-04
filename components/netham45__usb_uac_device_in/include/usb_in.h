#ifndef USB_IN_H
#define USB_IN_H

#include "sdkconfig.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// USB Audio configuration constants
#define USB_SAMPLE_RATE         CONFIG_USB_IN_SAMPLE_RATE
#define USB_CHANNEL_NUM         CONFIG_USB_IN_CHANNEL_COUNT
#define USB_BIT_DEPTH           CONFIG_USB_IN_BIT_DEPTH
#define USB_BYTES_PER_SAMPLE    (USB_BIT_DEPTH / 8)
#define USB_CHUNK_SIZE          CONFIG_USB_IN_CHUNK_SIZE
#define USB_BUFFER_SIZE         CONFIG_USB_IN_BUFFER_SIZE
#define USB_TASK_STACK_SIZE     CONFIG_USB_IN_TASK_STACK_SIZE
#define USB_TASK_PRIORITY       CONFIG_USB_IN_TASK_PRIORITY
#define USB_PCM_BUFFER_SIZE         CONFIG_USB_IN_PCM_BUFFER_SIZE

// External PCM buffer shared with network_out
extern RingbufHandle_t usb_in_pcm_buffer;

// Public API functions
esp_err_t usb_in_init(void (*init_done_cb)(void));
esp_err_t usb_in_start(void);
esp_err_t usb_in_stop(void);
void usb_in_deinit(void);
uint32_t usb_in_get_sample_rate(void);
bool usb_in_is_connected(void);

inline int usb_in_read(uint8_t *buffer, size_t size)
{
    if (!usb_in_pcm_buffer)
    {
        return 0;
    }
    size_t received_size = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
        usb_in_pcm_buffer, &received_size, pdMS_TO_TICKS(10), size);
    if (data && received_size > 0)
    {
        memcpy(buffer, data, received_size);
        vRingbufferReturnItem(usb_in_pcm_buffer, (void *)data);
        return received_size;
    }
    return 0;
}

static inline RingbufHandle_t usb_in_get_ringbuf()
{
    return usb_in_pcm_buffer;
}

#ifdef __cplusplus
}
#endif

#endif // USB_IN_H