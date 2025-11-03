#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define WAV_STREAM_MAX_CLIENTS 1
#define WAV_STREAM_BUFFER_MILLIS 25

esp_err_t wav_streamer_init(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels);
void wav_streamer_deinit(void);
esp_err_t wav_streamer_push(const uint8_t *pcm, size_t length);
esp_err_t wav_streamer_open(void);
void wav_streamer_close(void);
size_t wav_streamer_read(uint8_t *dest, size_t max_len, TickType_t timeout_ticks);
esp_err_t wav_streamer_build_header(uint8_t *dest, size_t dest_size, size_t *written);
bool wav_streamer_is_ready(void);
