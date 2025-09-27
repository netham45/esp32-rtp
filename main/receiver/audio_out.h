#pragma once
#include "config.h"
#include "esp_err.h"

// Forward declaration to avoid circular dependencies
typedef void* audio_device_handle_t;

// Audio output control functions
void process_audio_actions(bool is_startup);
void register_button(int button, void (*action)(bool, int, void *));
void setup_audio();

// Mode-agnostic playback functions
void start_playback(audio_device_handle_t handle);
void stop_playback();
void resume_playback();
bool is_playing();

// Audio data functions
void audio_write(uint8_t* data);
void audio_direct_write(uint8_t* data);

// Volume control
esp_err_t audio_out_update_volume(void);