#include "global.h"
#include "buffer.h"
#include "lifecycle_manager.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_out.h"
#include "config/config_manager.h"
#include "spdif_out.h"
#include "usb_out.h"
#include "sdkconfig.h"
#include "esp_timer.h"

// Low-rate summary interval default if not provided by Kconfig (declared in rtcp_receiver.c as well)
#ifndef CONFIG_AUDIO_OUT_LOG_SUMMARY_INTERVAL_MS
#define CONFIG_AUDIO_OUT_LOG_SUMMARY_INTERVAL_MS 10000
#endif
// USB out component now manages its own device handle internally
// We don't need to store the handle here anymore

bool playing = true;

uint8_t volume = 100;
uint8_t silence[32] = {0};
bool is_silent = false;
uint32_t silence_duration_ms = 0;
TickType_t last_audio_time = 0;

// Low-rate Audio structured summary; prints once per CONFIG_AUDIO_OUT_LOG_SUMMARY_INTERVAL_MS
static void audio_log_summary_if_due(void) {
    static uint64_t last_sum_us = 0;
    const uint64_t interval_us = ((uint64_t)CONFIG_AUDIO_OUT_LOG_SUMMARY_INTERVAL_MS) * 1000ULL;
    uint64_t now_us = esp_timer_get_time();
    if (last_sum_us != 0 && (now_us - last_sum_us) < interval_us) {
        return;
    }
    last_sum_us = now_us;

    // Compute time since last_audio_time in ms with tick rollover handling
    TickType_t current_tick = xTaskGetTickCount();
    uint32_t last_ms = 0;
    if (current_tick < last_audio_time) {
        last_ms = ((portMAX_DELAY - last_audio_time) + current_tick) * portTICK_PERIOD_MS;
    } else {
        last_ms = (current_tick - last_audio_time) * portTICK_PERIOD_MS;
    }

    device_mode_t mode = lifecycle_get_device_mode();
    const char *mode_str = (mode == MODE_RECEIVER_USB) ? "USB" :
                           (mode == MODE_RECEIVER_SPDIF) ? "SPDIF" : "UNKNOWN";

    // Only report silence ms when actually silent to match intent; otherwise 0
    uint32_t silent_ms = is_silent ? silence_duration_ms : 0;

    ESP_LOGI(TAG, "Audio sum: playing=%d mode=%s last_ms=%u silent_ms=%u",
             playing ? 1 : 0, mode_str, last_ms, silent_ms);
}
// Configuration change handler for audio output
esp_err_t audio_out_update_volume(void) {
    device_mode_t mode = lifecycle_get_device_mode();
    
    if (mode == MODE_RECEIVER_USB) {
        if (usb_out_is_connected() && playing) {
            float new_volume = lifecycle_get_volume();
            ESP_LOGI(TAG, "Updating USB volume to %.2f", new_volume);
            // usb_out_set_volume expects 0-100 range
            return usb_out_set_volume(new_volume);
        }
    } else if (mode == MODE_RECEIVER_SPDIF) {
        // SPDIF doesn't support volume control directly
        ESP_LOGD(TAG, "SPDIF output does not support volume control");
    }
    
    return ESP_OK;
}

bool is_playing() {
    return playing;
}

void resume_playback() {
    device_mode_t mode = lifecycle_get_device_mode();
    
    if (mode == MODE_RECEIVER_USB) {
        // Check if USB device is connected
        if (usb_out_is_connected()) {
            // USB out component is already configured and started
            // We just need to set the playing flag
            ESP_LOGI(TAG, "Resume Playback with USB DAC (SR: %" PRIu32 ", BD: %" PRIu8 ")",
                     lifecycle_get_sample_rate(), lifecycle_get_bit_depth());
            
            // Update volume to current setting
            usb_out_set_volume(lifecycle_get_volume());
            playing = true;
        } else {
            ESP_LOGI(TAG, "Cannot resume USB playback - No DAC connected");
            // Do NOT set playing to true if no DAC is available
        }
    } else if (mode == MODE_RECEIVER_SPDIF) {
        // For SPDIF mode, we just need to set playing flag
        ESP_LOGI(TAG, "Resume Playback for SPDIF output");
        playing = true;
    } else {
        ESP_LOGW(TAG, "Resume playback called in unsupported mode: %d", mode);
    }
}

void start_playback(audio_device_handle_t device_handle) {
    // With the refactored USB out component, we no longer need to store the handle
    // The USB out component manages it internally
    
    device_mode_t mode = lifecycle_get_device_mode();
    ESP_LOGI(TAG, "Starting playback in mode: %d", mode);
    
    if (mode == MODE_RECEIVER_USB) {
        ESP_LOGI(TAG, "USB receiver mode - checking connection");
        if (usb_out_is_connected()) {
            ESP_LOGI(TAG, "USB DAC connected and ready");
        } else {
            ESP_LOGW(TAG, "USB DAC not connected");
        }
    } else if (mode == MODE_RECEIVER_SPDIF) {
        ESP_LOGI(TAG, "SPDIF receiver mode - ready for output");
    }
    
    playing = true;
}

void stop_playback() {
    playing = false;
    ESP_LOGI(TAG, "Stop Playback");
    
    device_mode_t mode = lifecycle_get_device_mode();
    
    if (mode == MODE_RECEIVER_USB) {
        if (usb_out_is_connected()) {
            usb_out_stop_playback();
        }
    } else if (mode == MODE_RECEIVER_SPDIF) {
        // SPDIF doesn't need explicit stop
        ESP_LOGD(TAG, "SPDIF output stopped");
    }
}

void audio_direct_write(uint8_t *data) {
    // Reset silence tracking
    is_silent = false;
    silence_duration_ms = 0;
    last_audio_time = xTaskGetTickCount();
    
    device_mode_t mode = lifecycle_get_device_mode();
    
    if (mode == MODE_RECEIVER_USB) {
        // Check if USB device is connected before writing
        if (usb_out_is_connected()) {
            usb_out_write(data, PCM_CHUNK_SIZE, portMAX_DELAY);
        } else {
            // DAC is not connected - we should be in sleep mode
            ESP_LOGD(TAG, "Attempted USB write with no DAC");
        }
    } else if (mode == MODE_RECEIVER_SPDIF) {
        spdif_write(data, PCM_CHUNK_SIZE);
    } else {
        ESP_LOGW(TAG, "Direct write attempted in unsupported mode: %d", mode);
    }
}

void pcm_handler(void* pvParams) {
    // Initialize the last audio time to current time
    last_audio_time = xTaskGetTickCount();
    
    device_mode_t mode = lifecycle_get_device_mode();
    ESP_LOGI(TAG, "PCM handler started for mode: %d", mode);
    
    while (true) {
        // Periodic Audio summary (low rate)
        audio_log_summary_if_due();

        if (playing) {
            packet_with_ts_t *packet = pop_chunk();
            TickType_t current_time = xTaskGetTickCount();
            
            if (packet) {
                if (is_silent) {
                    is_silent = false;
                }
                silence_duration_ms = 0;
                last_audio_time = xTaskGetTickCount(); // Reset to current time
                
                // Validate skip_bytes doesn't exceed chunk size
                if (packet->skip_bytes >= PCM_CHUNK_SIZE) {
                    ESP_LOGE(TAG, "Invalid skip_bytes %u >= chunk size %d, dropping packet",
                            packet->skip_bytes, PCM_CHUNK_SIZE);
                    continue;
                }
                
                // Get audio start position and length based on skip_bytes
                uint8_t *audio_start = packet->packet_buffer + packet->skip_bytes;
                int audio_len = PCM_CHUNK_SIZE - packet->skip_bytes;
                
                if (packet->skip_bytes > 0) {
                    // Log every skip event with details
                    ESP_LOGI(TAG, "Audio trim: skipping %u bytes, playing %d bytes (%.2f ms trimmed)",
                            packet->skip_bytes, audio_len,
                            (float)packet->skip_bytes / 192.0f);  // 192 bytes/ms at 48kHz stereo 16-bit
                    
                    // Periodic summary
                    static uint32_t total_skipped_bytes = 0;
                    static uint32_t skip_count = 0;
                    total_skipped_bytes += packet->skip_bytes;
                    skip_count++;
                    
                    if (skip_count % 100 == 0) {
                        ESP_LOGI(TAG, "Trim summary: %u packets trimmed, avg %u bytes/packet (%.2f ms/packet)",
                                skip_count, total_skipped_bytes / skip_count,
                                (float)(total_skipped_bytes / skip_count) / 192.0f);
                    }
                }
                
                // Process the audio data based on current mode
                if (mode == MODE_RECEIVER_USB) {
                    if (usb_out_is_connected()) {
                        // USB output - handle partial chunks properly
                        if (audio_len > 0) {
                            usb_out_write(audio_start, audio_len, portMAX_DELAY);
                        } else {
                            ESP_LOGW(TAG, "No audio data to write after skipping %u bytes", packet->skip_bytes);
                        }
                    } else {
                        // DAC is not connected but we're trying to play - should enter sleep
                        ESP_LOGW(TAG, "PCM handler tried to write with no USB DAC");
                        playing = false; // Force playback to stop
                    }
                } else if (mode == MODE_RECEIVER_SPDIF) {
                    // SPDIF output - handle partial chunks properly
                    if (audio_len > 0) {
                        spdif_write(audio_start, audio_len);
                    } else {
                        ESP_LOGW(TAG, "No audio data to write after skipping %u bytes", packet->skip_bytes);
                    }
                } else {
                    ESP_LOGW(TAG, "PCM handler running in unsupported mode: %d", mode);
                }
            } else {
                // pop_chunk() returned NULL - NO PACKETS RECEIVED - THIS IS SILENCE!
                if (!is_silent) {
                    is_silent = true;
                    last_audio_time = current_time; // Start the silence timer
                }
                
                // Calculate how long we've been in silence, handling tick counter rollover
                // When current_time < last_audio_time, it means the counter has rolled over
                if (current_time < last_audio_time) {
                    // Handle rollover: calculate time until max value, then add time since 0
                    silence_duration_ms = ((portMAX_DELAY - last_audio_time) + current_time) * portTICK_PERIOD_MS;
                } else {
                    silence_duration_ms = (current_time - last_audio_time) * portTICK_PERIOD_MS;
                }
                
                // Only log occasionally to avoid spamming
                if (silence_duration_ms % 5000 == 0 && silence_duration_ms > 0) {
                    ESP_LOGI(TAG, "Silence duration: %" PRIu32 " ms", silence_duration_ms);
                }
                
                // Check if silence threshold is reached - use config value from lifecycle manager
                if (silence_duration_ms < 30000) {
                    if (silence_duration_ms >= lifecycle_get_silence_threshold_ms()) {
                        ESP_LOGI(TAG, "Silence threshold reached (%" PRIu32 " ms), entering sleep mode",
                                silence_duration_ms);
                        
                        // Trigger sleep mode
                        lifecycle_manager_post_event(LIFECYCLE_EVENT_ENTER_SLEEP);
                    }
                } else {
                    ESP_LOGI(TAG, "Absurd silence threshold ignored (%" PRIu32 " ms)",
                            silence_duration_ms);
                    last_audio_time = current_time;
                }
                  vTaskDelay(pdMS_TO_TICKS(0));
            }
        } else {
            // Not playing, wait longer
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void setup_audio() {
    device_mode_t mode = lifecycle_get_device_mode();
    ESP_LOGI(TAG, "Setting up audio for mode: %d", mode);
    
    // Create PCM handler task for all receiver modes
    if (mode == MODE_RECEIVER_USB || mode == MODE_RECEIVER_SPDIF) {
        xTaskCreatePinnedToCore(pcm_handler, "pcm_handler", 4096, NULL, 5, NULL, 1);
        ESP_LOGI(TAG, "PCM handler task created");
    }
}
