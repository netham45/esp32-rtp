#include "wav_streamer.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "string.h"

#ifndef PCM_CHUNK_SIZE
#include "../global.h"
#endif

#define STREAM_BUFFER_TRIGGER_LEVEL PCM_CHUNK_SIZE

typedef struct __attribute__((packed)) {
    char chunk_id[4];
    uint32_t chunk_size;
    char format[4];
    char subchunk1_id[4];
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char subchunk2_id[4];
    uint32_t subchunk2_size;
} wav_header_t;


static StreamBufferHandle_t s_stream_buffer = NULL;
static SemaphoreHandle_t s_client_lock = NULL;
static bool s_client_active = false;
static uint32_t s_sample_rate = 48000;
static uint8_t s_bit_depth = 16;
static uint8_t s_channels = 2;

static size_t compute_stream_buffer_size(void)
{
    size_t bytes_per_sample = s_bit_depth / 8;
    size_t bytes_per_ms = s_sample_rate * s_channels * bytes_per_sample / 1000;
    size_t desired = bytes_per_ms * WAV_STREAM_BUFFER_MILLIS;

    size_t min_size = PCM_CHUNK_SIZE * 2;
    if (desired < min_size) {
        desired = min_size;
    }

    if ((desired % PCM_CHUNK_SIZE) != 0) {
        desired += PCM_CHUNK_SIZE - (desired % PCM_CHUNK_SIZE);
    }

    return desired;
}

static esp_err_t ensure_stream_buffer(void)
{
    if (s_stream_buffer) {
        return ESP_OK;
    }

    size_t buffer_size = compute_stream_buffer_size();
    s_stream_buffer = xStreamBufferCreate(buffer_size, STREAM_BUFFER_TRIGGER_LEVEL);
    if (!s_stream_buffer) {
        ESP_LOGE(TAG, "Failed to allocate stream buffer (%zu bytes)", buffer_size);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wav_streamer_init(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels)
{
    s_sample_rate = sample_rate;
    s_bit_depth = bit_depth;
    s_channels = channels;

    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
        if (!s_client_lock) {
            ESP_LOGE(TAG, "Failed to create client mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "Free heap before initializing WAV buffer: %u bytes", esp_get_free_heap_size());
    return ensure_stream_buffer();
}

void wav_streamer_deinit(void)
{
    if (s_client_lock) {
        xSemaphoreTake(s_client_lock, portMAX_DELAY);
        s_client_active = false;
        xSemaphoreGive(s_client_lock);
    }

    if (s_stream_buffer) {
        vStreamBufferDelete(s_stream_buffer);
        s_stream_buffer = NULL;
    }
}

bool wav_streamer_is_ready(void)
{
    return s_stream_buffer != NULL;
}

esp_err_t wav_streamer_push(const uint8_t *pcm, size_t length)
{
    if (!s_stream_buffer || !pcm || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = xStreamBufferSend(s_stream_buffer, pcm, length, 0);
    if (written != length) {
        ESP_LOGV(TAG, "Stream buffer full, dropped %zu bytes", length - written);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wav_streamer_open(void)
{
    if (!s_stream_buffer || !s_client_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_FAIL;

    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (!s_client_active) {
            s_client_active = true;
            xStreamBufferReset(s_stream_buffer);
            err = ESP_OK;
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(s_client_lock);
    } else {
        err = ESP_ERR_TIMEOUT;
    }

    return err;
}

void wav_streamer_close(void)
{
    if (!s_client_lock) {
        return;
    }

    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_client_active = false;
        xSemaphoreGive(s_client_lock);
    }
}

size_t wav_streamer_read(uint8_t *dest, size_t max_len, TickType_t timeout_ticks)
{
    if (!s_stream_buffer || !dest || max_len == 0) {
        return 0;
    }

    return xStreamBufferReceive(s_stream_buffer, dest, max_len, timeout_ticks);
}

esp_err_t wav_streamer_build_header(uint8_t *dest, size_t dest_size, size_t *written)
{
    if (!dest || dest_size < sizeof(wav_header_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    wav_header_t header;
    memset(&header, 0, sizeof(header));

    size_t bytes_per_sample = s_bit_depth / 8;
    uint32_t byte_rate = s_sample_rate * s_channels * bytes_per_sample;
    uint16_t block_align = s_channels * bytes_per_sample;

    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = 0xFFFFFFFF;
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1;
    header.num_channels = s_channels;
    header.sample_rate = s_sample_rate;
    header.byte_rate = byte_rate;
    header.block_align = block_align;
    header.bits_per_sample = s_bit_depth;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = 0xFFFFFFFF;

    memcpy(dest, &header, sizeof(header));
    if (written) {
        *written = sizeof(header);
    }

    return ESP_OK;
}
