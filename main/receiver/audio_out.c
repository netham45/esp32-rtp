#include "global.h"
#include "buffer.h"
#include "lifecycle_manager.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <sys/time.h>  // For gettimeofday()
#include "audio_out.h"
#include "config/config_manager.h"
#include "spdif_out.h"
#include "usb_out.h"

// USB out component now manages its own device handle internally
// We don't need to store the handle here anymore

bool playing = true;

uint8_t volume = 100;
uint8_t silence[32] = {0};
bool is_silent = false;
uint32_t silence_duration_ms = 0;
uint64_t last_audio_time = 0;

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
    last_audio_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
    
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
    // Initialize the last audio time to current time (in milliseconds)
    last_audio_time = esp_timer_get_time() / 1000;
    
    device_mode_t mode = lifecycle_get_device_mode();
    ESP_LOGI(TAG, "PCM handler started for mode: %d with timestamp support", mode);
    
    uint32_t loop_counter = 0;
    uint32_t pop_attempts = 0;
    uint32_t samples_consumed = 0;
    uint32_t no_data_count = 0;
    uint32_t waiting_count = 0;
    uint64_t last_log_time_ms = esp_timer_get_time() / 1000;
    
    while (true) {
        loop_counter++;
        
        // Log stats every 5 seconds
        uint64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_log_time_ms) >= 5000) {
            ESP_LOGI(TAG, "PCM handler stats: loops=%lu, pop_attempts=%lu, consumed=%lu, no_data=%lu, waiting=%lu, playing=%d",
                     loop_counter, pop_attempts, samples_consumed, no_data_count, waiting_count, playing);
            last_log_time_ms = now_ms;
            loop_counter = 0;
            pop_attempts = 0;
            samples_consumed = 0;
            no_data_count = 0;
            waiting_count = 0;
        }
        
        if (playing) {
            // Use enhanced pop function that considers timestamps
            pop_attempts++;
            
            // Log before attempting to pull from buffer
            static uint32_t pre_pop_counter = 0;
            pre_pop_counter++;
            if (pre_pop_counter <= 5 || pre_pop_counter % 500 == 0) {
                ESP_LOGI(TAG, "📥 Attempting buffer pop #%lu", pop_attempts);
            }
            
            pop_result_t result = pop_chunk_timed();
            
            // Get actual system time in milliseconds (set by NTP)
            struct timeval tv;
            gettimeofday(&tv, NULL);
            uint64_t current_time_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
            
            // Log detailed result every 100 attempts
            if (pop_attempts % 100 == 0) {
                ESP_LOGD(TAG, "pop_chunk_timed result: data=%p, should_play_now=%d, playout_time_ms=%llu",
                         result.data, result.should_play_now, result.playout_time_ms);
            }
            
            if (result.data && result.should_play_now) {
                samples_consumed++;
                
                // Log when we successfully pull audio from buffer
                static uint32_t buffer_pull_counter = 0;
                buffer_pull_counter++;
                
                // Log frequently at first, then less often
                bool should_log = (buffer_pull_counter <= 10) ||
                                 (buffer_pull_counter % 50 == 0);
                
                if (should_log) {
                    ESP_LOGI(TAG, "✓ Audio pulled from buffer #%lu (total consumed: %lu, data ptr: %p)",
                             buffer_pull_counter, samples_consumed, result.data);
                }
                
                if (is_silent) {
                    ESP_LOGI(TAG, "Audio data received - exiting silence state");
                    is_silent = false;
                }
                silence_duration_ms = 0;
                last_audio_time = current_time_ms; // Reset to current time in milliseconds
                
                // Log if we're using synchronized playout
                static uint32_t sync_log_counter = 0;
                if (result.playout_time_ms > 0) {
                    // Check for corrupted timestamp values
                    if (result.playout_time_ms > 0xFFFFFFFF00000000ULL) {
                        static uint32_t corrupt_count = 0;
                        corrupt_count++;
                        ESP_LOGE(TAG, "CORRUPTED playout timestamp detected: %llu (0x%llX), count: %lu - ignoring sync",
                                result.playout_time_ms, result.playout_time_ms, corrupt_count);
                        result.playout_time_ms = 0;  // Treat as unsynchronized
                    } else if (++sync_log_counter % 100 == 0) {
                        // Also show boot time for comparison
                        uint64_t boot_time_ms = esp_timer_get_time() / 1000;
                        ESP_LOGI(TAG, "Playing synchronized audio (playout_time: %llu ms, system_time: %llu ms, boot_time: %llu ms, consumed: %lu)",
                                result.playout_time_ms, current_time_ms, boot_time_ms, samples_consumed);
                    }
                }
                
                // Process the audio data based on current mode
                if (mode == MODE_RECEIVER_USB) {
                    if (usb_out_is_connected()) {
                        // Log USB writes periodically
                        static uint32_t usb_write_counter = 0;
                        usb_write_counter++;
                        if (usb_write_counter <= 5 || usb_write_counter % 100 == 0) {
                            ESP_LOGI(TAG, "→ USB: Writing %d bytes to DAC (write #%lu)",
                                     PCM_CHUNK_SIZE, usb_write_counter);
                        }
                        usb_out_write(result.data, PCM_CHUNK_SIZE, portMAX_DELAY);
                    } else {
                        // DAC is not connected but we're trying to play - should enter sleep
                        ESP_LOGW(TAG, "PCM handler tried to write with no USB DAC");
                        playing = false; // Force playback to stop
                    }
                } else if (mode == MODE_RECEIVER_SPDIF) {
                    // Log SPDIF writes periodically
                    static uint32_t spdif_write_counter = 0;
                    spdif_write_counter++;
                    if (spdif_write_counter <= 5 || spdif_write_counter % 100 == 0) {
                        ESP_LOGI(TAG, "→ SPDIF: Writing %d bytes (write #%lu)",
                                 PCM_CHUNK_SIZE, spdif_write_counter);
                    }
                    spdif_write(result.data, PCM_CHUNK_SIZE);
                } else {
                    ESP_LOGW(TAG, "PCM handler running in unsupported mode: %d", mode);
                }
            } else if (!result.data) {
                no_data_count++;
                // pop_chunk_timed() returned no data - NO PACKETS RECEIVED - THIS IS SILENCE!
                if (!is_silent) {
                    ESP_LOGI(TAG, "Entering silence state - no data from pop_chunk_timed");
                    is_silent = true;
                    last_audio_time = current_time_ms; // Start the silence timer in milliseconds
                }
                
                // Calculate how long we've been in silence (now using milliseconds directly)
                silence_duration_ms = current_time_ms - last_audio_time;
                
                // Log more frequently during initial silence
                uint32_t log_interval = (silence_duration_ms < 10000) ? 1000 : 5000;
                if (silence_duration_ms % log_interval == 0 && silence_duration_ms > 0) {
                    ESP_LOGI(TAG, "Silence duration: %" PRIu32 " ms (no_data_count=%lu)",
                             silence_duration_ms, no_data_count);
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
                    last_audio_time = current_time_ms;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                waiting_count++;
                // Data available but not ready to play yet (waiting for scheduled time)
                if (waiting_count % 1000 == 0) {
                    ESP_LOGI(TAG, "Data available but waiting for scheduled playout (count=%lu)", waiting_count);
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else {
            // Not playing, wait longer
            static uint32_t not_playing_log_count = 0;
            if (++not_playing_log_count % 50 == 0) {
                ESP_LOGD(TAG, "PCM handler not playing, waiting... (count=%lu)", not_playing_log_count);
            }
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
