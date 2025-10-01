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
#include "usb/uac_host.h"

// Device handle storage - only one will be used at a time based on mode
static audio_device_handle_t audio_device_handle = NULL;

bool playing = true;

uint8_t volume = 100;
uint8_t silence[32] = {0};
bool is_silent = false;
uint32_t silence_duration_ms = 0;
TickType_t last_audio_time = 0;

// Configuration change handler for audio output
esp_err_t audio_out_update_volume(void) {
    device_mode_t mode = lifecycle_get_device_mode();
    
    if (mode == MODE_RECEIVER_USB) {
        if (audio_device_handle != NULL && playing) {
            float new_volume = lifecycle_get_volume();
            ESP_LOGI(TAG, "Updating USB volume to %.2f", new_volume);
            uac_host_device_handle_t usb_handle = (uac_host_device_handle_t)audio_device_handle;
            return uac_host_device_set_volume(usb_handle, new_volume * 100.0f);
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
        // Only try to resume if we have a valid DAC handle
        if (audio_device_handle != NULL) {
            // Get current configuration from lifecycle manager
            uac_host_stream_config_t stm_config = {
                .channels = 2,
                .bit_resolution = lifecycle_get_bit_depth(),
                .sample_freq = lifecycle_get_sample_rate(),
            };
            ESP_LOGI(TAG, "Resume Playback with USB DAC (SR: %" PRIu32 ", BD: %" PRIu8 ")",
                     lifecycle_get_sample_rate(), lifecycle_get_bit_depth());
            
            uac_host_device_handle_t usb_handle = (uac_host_device_handle_t)audio_device_handle;
            ESP_ERROR_CHECK(uac_host_device_start(usb_handle, &stm_config));
            ESP_ERROR_CHECK(uac_host_device_set_volume(usb_handle, lifecycle_get_volume() * 100.0f));
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
    audio_device_handle = device_handle;
    
    device_mode_t mode = lifecycle_get_device_mode();
    ESP_LOGI(TAG, "Starting playback in mode: %d", mode);
    
    if (mode == MODE_RECEIVER_USB) {
        ESP_LOGI(TAG, "USB receiver mode - device handle set");
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
        if (audio_device_handle != NULL) {
            uac_host_device_handle_t usb_handle = (uac_host_device_handle_t)audio_device_handle;
            uac_host_device_stop(usb_handle);
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
        // Check if we have a valid DAC handle before writing
        if (audio_device_handle != NULL) {
            uac_host_device_handle_t usb_handle = (uac_host_device_handle_t)audio_device_handle;
            uac_host_device_write(usb_handle, data, PCM_CHUNK_SIZE, portMAX_DELAY);
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
        if (playing) {
            uint8_t *data = pop_chunk();
            TickType_t current_time = xTaskGetTickCount();
            
            if (data) {
                if (is_silent) {
                    is_silent = false;
                }
                silence_duration_ms = 0;
                last_audio_time = xTaskGetTickCount(); // Reset to current time
                
                // Process the audio data based on current mode
                if (mode == MODE_RECEIVER_USB) {
                    if (audio_device_handle != NULL) {
                        uac_host_device_handle_t usb_handle = (uac_host_device_handle_t)audio_device_handle;
                        uac_host_device_write(usb_handle, data, PCM_CHUNK_SIZE, portMAX_DELAY);
                    } else {
                        // DAC is not connected but we're trying to play - should enter sleep
                        ESP_LOGW(TAG, "PCM handler tried to write with no USB DAC");
                        playing = false; // Force playback to stop
                    }
                } else if (mode == MODE_RECEIVER_SPDIF) {
                    spdif_write(data, PCM_CHUNK_SIZE);
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
        xTaskCreatePinnedToCore(pcm_handler, "pcm_handler", 2048, NULL, 5, NULL, 1);
        ESP_LOGI(TAG, "PCM handler task created");
    }
}
