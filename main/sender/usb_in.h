#ifndef USB_IN_H
#define USB_IN_H

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
#define USB_SAMPLE_RATE         48000      // 48kHz sample rate
#define USB_CHANNEL_NUM         2           // Stereo
#define USB_BIT_DEPTH           16          // 16-bit audio
#define USB_BYTES_PER_SAMPLE    2           // 2 bytes per sample (16-bit)
#define USB_CHUNK_SIZE          1152        // Matches network packet size
#define USB_BUFFER_SIZE         (USB_CHUNK_SIZE * 2)  // 8 chunks buffered
#define USB_TASK_STACK_SIZE     4096        // 4KB stack for USB task
#define USB_TASK_PRIORITY       5          // High priority for audio
#define PCM_BUFFER_SIZE         8192        // 8KB PCM ring buffer

// External PCM buffer shared with network_out
static RingbufHandle_t usb_in_pcm_buffer = NULL;

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