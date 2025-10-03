# ESP32 USB Audio Class (UAC) 1.0 Host Output

High-performance USB audio output driver implementing USB Audio Class 1.0 host mode for ESP32 devices. Enables seamless audio streaming to USB speakers, DACs, headphones, and other UAC-compliant audio devices with configurable sample rates, bit depths, and volume control.

Key files:
- [include/usb_out.h](include/usb_out.h)
- [usb_out.c](usb_out.c)
- [idf_component.yml](idf_component.yml)
- [CMakeLists.txt](CMakeLists.txt)

Supported targets and IDF:
- ESP-IDF [>= 5.0] (USB host stack required)
- Targets: esp32s2, esp32s3, esp32p4 (USB OTG capable chips only)
- Repository: [https://github.com/netham45/esp32-usb-out](https://github.com/netham45/esp32-usb-out)
- Version: 1.0.0


## Data Flow

```mermaid
flowchart LR
  APP[Application] --> PCM[PCM Audio Data]
  PCM --> WRITE[usb_out_write()]
  WRITE --> UAC[UAC Host Driver]
  UAC --> USB[USB Host Stack]
  USB --> DEV[USB Audio Device]
  DEV --> SPK[Speakers/DAC]
```


## Features
- **USB UAC 1.0 Host Mode**: Full implementation of USB Audio Class 1.0 host driver
- **Flexible Audio Formats**: Configurable sample rates (8kHz-192kHz) and bit depths (16/24/32-bit)
- **Volume Control**: Software-based volume adjustment with [`usb_out_set_volume()`](include/usb_out.h#L28)
- **Device Management**: Automatic USB device enumeration and connection handling
- **Sleep Mode Support**: Graceful suspend/resume with [`usb_out_prepare_for_sleep()`](include/usb_out.h#L32) and [`usb_out_restore_after_wake()`](include/usb_out.h#L33)
- **Error Recovery**: Built-in timeout detection and enumeration error handling
- **Non-blocking Writes**: Configurable timeout for audio data writes
- **Connection Status**: Real-time connection monitoring via [`usb_out_is_connected()`](include/usb_out.h#L21)


## Hardware Requirements
- **USB OTG Support**: ESP32-S2, ESP32-S3, or ESP32-P4 with USB OTG peripheral
- **USB Connector**: USB-A host connector or USB-C with appropriate CC resistors for host mode
- **Power Supply**: Sufficient current capacity to power USB audio devices (typically 500mA minimum)
- **GPIO Pins**: USB D+ and D- pins (typically GPIO19/20 on ESP32-S2/S3)


## Quick Start

### 1) Initialize the USB host and start audio output

```c
#include "usb_out.h"

void app_main(void) {
    // Initialize USB host stack
    ESP_ERROR_CHECK(usb_out_init());                              // [usb_out_init()](include/usb_out.h#L16)
    
    // Start audio output with 48kHz, 16-bit, 75% volume
    ESP_ERROR_CHECK(usb_out_start(48000, 16, 0.75f));            // [usb_out_start()](include/usb_out.h#L17)
    
    // Wait for device connection
    while (!usb_out_is_connected()) {                             // [usb_out_is_connected()](include/usb_out.h#L21)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "USB audio device connected!");
}
```

### 2) Stream audio data

```c
// Write PCM audio data
uint8_t pcm_buffer[USB_OUT_PCM_CHUNK_SIZE];
size_t bytes_to_write = sizeof(pcm_buffer);

// Fill pcm_buffer with audio data...

esp_err_t ret = usb_out_write(pcm_buffer, bytes_to_write,      // [usb_out_write()](include/usb_out.h#L24)
                              pdMS_TO_TICKS(100));
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write audio data: %s", esp_err_to_name(ret));
}
```

### 3) Volume control

```c
// Set volume to 50%
ESP_ERROR_CHECK(usb_out_set_volume(0.5f));                      // [usb_out_set_volume()](include/usb_out.h#L28)

// Get current volume
float current_volume;
ESP_ERROR_CHECK(usb_out_get_volume(&current_volume));          // [usb_out_get_volume()](include/usb_out.h#L29)
ESP_LOGI(TAG, "Current volume: %.1f%%", current_volume * 100);
```

### 4) Stop and cleanup

```c
// Stop playback
ESP_ERROR_CHECK(usb_out_stop_playback());                       // [usb_out_stop_playback()](include/usb_out.h#L25)

// Stop the USB output
ESP_ERROR_CHECK(usb_out_stop());                                // [usb_out_stop()](include/usb_out.h#L18)

// Deinitialize when done
ESP_ERROR_CHECK(usb_out_deinit());                              // [usb_out_deinit()](include/usb_out.h#L19)
```


## API Reference

### Initialization and Control
- [`usb_out_init()`](include/usb_out.h#L16): Initialize USB host stack and prepare for audio device connection
- [`usb_out_start()`](include/usb_out.h#L17): Start USB audio output with specified parameters (sample rate, bit depth, initial volume)
- [`usb_out_stop()`](include/usb_out.h#L18): Stop USB audio output and release resources
- [`usb_out_deinit()`](include/usb_out.h#L19): Complete deinitialization and cleanup of USB host stack

### Device Management
- [`usb_out_get_device_handle()`](include/usb_out.h#L20): Get the UAC device handle for direct access to device properties
- [`usb_out_is_connected()`](include/usb_out.h#L21): Check if a USB audio device is currently connected and ready

### Audio Operations
- [`usb_out_write()`](include/usb_out.h#L24): Write PCM audio data to the USB device with timeout
- [`usb_out_stop_playback()`](include/usb_out.h#L25): Immediately stop audio playback without disconnecting

### Volume Control
- [`usb_out_set_volume()`](include/usb_out.h#L28): Set output volume (0.0 to 1.0)
- [`usb_out_get_volume()`](include/usb_out.h#L29): Get current volume level

### Sleep Mode Support
- [`usb_out_prepare_for_sleep()`](include/usb_out.h#L32): Prepare USB subsystem for deep sleep
- [`usb_out_restore_after_wake()`](include/usb_out.h#L33): Restore USB functionality after waking from sleep

### Error Handling
- [`usb_out_check_enumeration_timeout()`](include/usb_out.h#L36): Check and handle device enumeration timeouts


## Audio Format Support

### Supported Sample Rates
- Standard rates: 8000, 11025, 16000, 22050, 44100, 48000, 88200, 96000, 176400, 192000 Hz
- Device-dependent: Actual support depends on connected USB audio device capabilities

### Bit Depths
- 16-bit PCM (most common)
- 24-bit PCM
- 32-bit PCM
- Format must match the connected device's capabilities

### Channel Configuration
- Stereo (2 channels) - default and most widely supported
- Mono can be supported depending on device


## Configuration Constants

- [`USB_OUT_PCM_CHUNK_SIZE`](include/usb_out.h#L10): Default 1024 bytes - optimal chunk size for USB transfers


## Example Usage Scenarios

### Music Playback System

```c
// High-quality music streaming setup
void setup_music_player(void) {
    usb_out_init();
    
    // CD-quality audio: 44.1kHz, 16-bit
    usb_out_start(44100, 16, 0.8f);
    
    while (music_playing) {
        // Get next audio chunk from decoder
        size_t chunk_size = decoder_get_pcm(pcm_buffer, USB_OUT_PCM_CHUNK_SIZE);
        
        if (chunk_size > 0) {
            usb_out_write(pcm_buffer, chunk_size, portMAX_DELAY);
        }
    }
}
```

### Network Audio Receiver

```c
// RTP/Network audio streaming
void network_audio_task(void *pvParameters) {
    usb_out_init();
    usb_out_start(48000, 16, 1.0f);
    
    while (1) {
        // Receive audio from network
        if (network_receive_audio(pcm_buffer, &size, timeout) == ESP_OK) {
            // Forward to USB output
            usb_out_write(pcm_buffer, size, pdMS_TO_TICKS(10));
        }
    }
}
```

### Audio Effects Processor

```c
// Real-time audio processing
void audio_processor(void) {
    usb_out_init();
    usb_out_start(48000, 24, 0.9f);  // Higher bit depth for processing headroom
    
    while (processing_enabled) {
        // Get input audio
        input_source_read(input_buffer, BUFFER_SIZE);
        
        // Apply DSP effects
        apply_reverb(input_buffer, processed_buffer);
        apply_eq(processed_buffer, output_buffer);
        
        // Output to USB device
        usb_out_write(output_buffer, BUFFER_SIZE, pdMS_TO_TICKS(5));
    }
}
```


## Volume Control Usage

The volume control operates in software before sending audio to the USB device:

```c
// Gradual volume fade
void fade_volume(float start_vol, float end_vol, int duration_ms) {
    int steps = duration_ms / 10;
    float step_size = (end_vol - start_vol) / steps;
    
    for (int i = 0; i < steps; i++) {
        float volume = start_vol + (step_size * i);
        usb_out_set_volume(volume);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    usb_out_set_volume(end_vol);
}

// Mute/unmute implementation
void toggle_mute(void) {
    static float saved_volume = 0.5f;
    static bool muted = false;
    
    if (muted) {
        usb_out_set_volume(saved_volume);
    } else {
        usb_out_get_volume(&saved_volume);
        usb_out_set_volume(0.0f);
    }
    muted = !muted;
}
```


## Sleep Mode Handling

Proper sleep mode handling ensures USB devices are safely suspended and restored:

```c
// Power management implementation
void enter_deep_sleep(void) {
    ESP_LOGI(TAG, "Preparing USB for sleep...");
    
    // Stop playback and prepare USB
    usb_out_stop_playback();
    usb_out_prepare_for_sleep();
    
    // Enter deep sleep
    esp_deep_sleep_start();
}

void wake_from_sleep(void) {
    ESP_LOGI(TAG, "Restoring USB after wake...");
    
    // Restore USB functionality
    usb_out_restore_after_wake();
    
    // Wait for device reconnection
    int retry = 0;
    while (!usb_out_is_connected() && retry++ < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (usb_out_is_connected()) {
        ESP_LOGI(TAG, "USB audio device reconnected");
        // Resume playback...
    }
}
```


## Error Recovery and Device Enumeration

Handle device connection issues and enumeration timeouts:

```c
// Robust connection handling
void usb_connection_monitor_task(void *pvParameters) {
    bool was_connected = false;
    
    while (1) {
        bool is_connected = usb_out_is_connected();
        
        if (is_connected != was_connected) {
            if (is_connected) {
                ESP_LOGI(TAG, "USB audio device connected");
                
                // Get device info
                uac_host_device_handle_t handle = usb_out_get_device_handle();
                if (handle != NULL) {
                    // Query device capabilities...
                }
            } else {
                ESP_LOGW(TAG, "USB audio device disconnected");
                
                // Check for enumeration timeout
                if (usb_out_check_enumeration_timeout() == ESP_ERR_TIMEOUT) {
                    ESP_LOGE(TAG, "Device enumeration timeout - resetting USB");
                    usb_out_stop();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    usb_out_init();
                    usb_out_start(48000, 16, 0.75f);
                }
            }
            was_connected = is_connected;
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```


## Dependencies

This component depends on:
- `espressif/usb_host_uac` version ^1.0.* - USB Audio Class host driver
- ESP-IDF USB Host stack (built-in for supported targets)
- FreeRTOS (included with ESP-IDF)

Specified in [`idf_component.yml`](idf_component.yml):
```yaml
dependencies:
  espressif/usb_host_uac: "^1.0.*"
```


## Threading and Resources

- USB host task runs with high priority for reliable USB communication
- Audio writes are thread-safe and can be called from any task
- DMA transfers handle the actual USB communication efficiently
- Typical memory usage: ~8KB for buffers and USB stack


## Limitations

- Only UAC 1.0 devices are supported (UAC 2.0/3.0 devices may work in backward compatibility mode)
- No hardware volume control - volume adjustment is done in software
- Sample rate conversion is not performed - output rate must match device capability
- Maximum of one USB audio device connected at a time
- No support for USB hubs - device must be directly connected


## Troubleshooting

### Device Not Detected
- Verify USB OTG cable is properly connected and supports host mode
- Check power supply can provide sufficient current (500mA minimum)
- Ensure device is UAC 1.0 compliant
- Try different USB devices to isolate hardware issues

### No Audio Output
- Confirm sample rate and bit depth match device capabilities
- Check volume is not set to 0.0
- Verify PCM data format matches configuration (endianness, channels)
- Monitor return values from `usb_out_write()` for errors

### Intermittent Audio/Dropouts
- Increase write timeout value in `usb_out_write()`
- Ensure consistent data flow without gaps
- Check CPU usage - USB host requires processing time
- Verify power supply stability

### Connection Issues
```c
// Debug connection problems
void debug_usb_connection(void) {
    esp_log_level_set("USB_HOST", ESP_LOG_DEBUG);
    esp_log_level_set("UAC_HOST", ESP_LOG_DEBUG);
    
    if (!usb_out_is_connected()) {
        ESP_LOGE(TAG, "No USB device connected");
        
        // Check enumeration timeout
        if (usb_out_check_enumeration_timeout() == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Enumeration timeout detected");
        }
    }
}
```

### Volume Control Not Working
- Verify volume parameter is between 0.0 and 1.0
- Some devices may not respond to software volume control
- Check audio data is actually being written after volume change


## Performance Considerations

- Write operations should be called regularly to prevent underruns


## Version and Metadata
- Component version: 1.0.0
- Repository: [esp32-usb-out](https://github.com/netham45/esp32-usb-out)
- Supported ESP-IDF: >= 5.0
- License: Refer to repository for licensing information