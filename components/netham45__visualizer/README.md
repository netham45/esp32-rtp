# ESP32 LED Audio Visualizer

Real-time audio visualization system for WS2812B LED strips with PPM metering, multiple color schemes, and auto-gain control. Processes 16-bit stereo PCM audio at 48kHz to create dynamic LED displays synchronized to music.

## Overview

This component provides a complete audio visualization solution using WS2812B addressable LED strips. It consists of three tightly integrated modules that work together to analyze PCM audio data and generate stunning visual effects on a 64×2 LED matrix display.

**Repository:** https://github.com/netham45/esp32-visualizer  
**Version:** 1.0.0  
**Supported targets:** esp32, esp32s2, esp32s3, esp32p4  
**ESP-IDF:** >= 5.0

## Architecture

```mermaid
flowchart TB
    subgraph Application Layer
        APP[Application]
    end
    
    subgraph Visualizer Component
        VT[Visualizer Task<br/>visualizer_task.h]
        PCM[PCM Visualizer<br/>pcm_visualizer.h]
        LED[LED Controller<br/>led_strip_controller.h]
        
        VT --> PCM
        VT --> LED
        PCM -.->|Loudness Data| LED
    end
    
    subgraph Hardware
        STRIP[WS2812B LED Strip<br/>128 LEDs 64×2]
    end
    
    APP -->|visualizer_feed_pcm()| VT
    APP -->|Control APIs| VT
    VT -->|PCM Audio| PCM
    PCM -->|PPM/Loudness| VT
    VT -->|Visualization| LED
    LED -->|RMT Driver| STRIP
```

### Module Responsibilities

1. **Visualizer Task** ([`visualizer_task.h`](include/visualizer_task.h)) - Main coordinator
   - Manages initialization and lifecycle
   - Routes PCM data to analyzer
   - Coordinates LED updates
   - Handles suspend/resume for flash operations

2. **PCM Visualizer** ([`pcm_visualizer.h`](include/pcm_visualizer.h)) - Audio analysis engine
   - Processes 16-bit stereo PCM at 48kHz
   - Calculates PPM levels with peak hold
   - Implements auto-gain control
   - Provides loudness/dB measurements

3. **LED Strip Controller** ([`led_strip_controller.h`](include/led_strip_controller.h)) - Visual output
   - Controls 128 WS2812B LEDs (skipping LED #0)
   - Multiple color schemes (rainbow, heat, monochrome)
   - Smooth transitions and animations
   - Peak hold visualization

## Hardware Requirements

### LED Strip Specifications
- **Type:** WS2812B addressable RGB LEDs
- **Count:** 129 LEDs total (128 active, LED #0 skipped)
- **Layout:** 64 columns × 2 rows
- **Data line:** Single GPIO pin with 5V level shifter recommended
- **Power:** 5V supply, ~7.7A max (60mA per LED at full white)

### Power Supply Requirements
```
Power calculation:
- Maximum current: 128 LEDs × 60mA = 7.68A
- Recommended PSU: 5V 10A (with headroom)
- Voltage drop mitigation: Power injection every 30-50 LEDs
```

### Wiring Diagram
```
                ESP32                    Level Shifter           WS2812B Strip
                                         (3.3V → 5V)
    ┌──────────────────┐              ┌─────────────┐        ┌──────────────┐
    │                  │              │             │        │              │
    │        GPIO (TX)├──────────────┤LV        HV├────────┤DIN           │
    │                  │              │             │        │              │
    │             3.3V├──────────────┤LV VCC       │    ┌───┤5V            │
    │                  │          ┌───┤HV VCC       │    │   │              │
    │              GND├──────┬────┴───┤GND      GND├────┴───┤GND           │
    │                  │      │        │             │        │              │
    └──────────────────┘      │        └─────────────┘        └──────────────┘
                              │                                      │
                              │         5V Power Supply              │
                              │        ┌─────────────┐               │
                              └────────┤-         +5V├───────────────┘
                                       └─────────────┘
```

## LED Strip Physical Layout

The visualizer uses a unique 64×2 LED matrix configuration optimized for stereo audio visualization:

```
Physical Layout (Top View):
┌─────────────────────────────────────────────────────────────────┐
│  Left Channel (32 columns)  │  Right Channel (32 columns)       │
├─────────────────────────────────────────────────────────────────┤
│ Row 0: LEDs 1-32            │ Row 0: LEDs 33-64                │
│ Row 1: LEDs 65-96           │ Row 1: LEDs 97-128               │
└─────────────────────────────────────────────────────────────────┘

LED Indexing Formula:
- LED Index = column + (row × 64) + 1
- LED #0 is skipped (reserved for ESP32 built-in LED)
- Columns 0-31: Left audio channel
- Columns 32-63: Right audio channel
```

### Visualization Modes

1. **VU Meter Mode** - Classic stereo level meters
   ```
   Left Channel  [████████████░░░░░░░░░░░░░░░░░░░]
   Right Channel [██████████████████░░░░░░░░░░░░░]
   ```

## Quick Start

### Basic Setup and Usage

```c
#include "visualizer_task.h"

// 1. Initialize the visualizer system
esp_err_t err = visualizer_init();
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init visualizer: %s", esp_err_to_name(err));
    return err;
}

// 2. Configure visualization settings
visualizer_set_color_scheme(LED_COLOR_RAINBOW);
visualizer_set_smoothing(0.7f);
visualizer_set_peak_hold(true);
visualizer_set_gain(1.0f);

// 3. Feed PCM audio data (16-bit stereo, 48kHz)
int16_t pcm_buffer[1024]; // Interleaved stereo: L0,R0,L1,R1,...
// ... receive or generate audio ...
visualizer_feed_pcm((uint8_t*)pcm_buffer, sizeof(pcm_buffer));

// 4. Query statistics
visualizer_stats_t stats;
visualizer_get_stats(&stats);
ESP_LOGI(TAG, "Samples: %lu, LED updates: %lu", 
         stats.samples_processed, stats.led_updates);

// 5. Suspend for flash operations
visualizer_suspend();  // Safe to write to NVS
// ... perform flash operations ...
visualizer_resume();   // Resume visualization

// 6. Cleanup when done
visualizer_deinit();
```

## API Reference

### Visualizer Task API

#### Core Control Functions

##### `esp_err_t visualizer_init(void)`
Initialize the complete visualization system including PCM analyzer and LED controller.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if memory allocation fails
- Error code from subsystem initialization

**Example:**
```c
if (visualizer_init() != ESP_OK) {
    ESP_LOGE(TAG, "Visualizer init failed");
}
```

##### `esp_err_t visualizer_deinit(void)`
Shut down the visualization system and free all resources.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized

##### `esp_err_t visualizer_feed_pcm(const uint8_t* data, size_t len)`
Feed PCM audio data to the visualizer for processing.

**Parameters:**
- `data`: Pointer to 16-bit stereo PCM data
- `len`: Length in bytes (must be multiple of 4)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized
- `ESP_ERR_NO_MEM` if buffer is full

**Data Format:**
- 16-bit signed integers (int16_t)
- Stereo interleaved: [L0,R0,L1,R1,...]
- Sample rate: 48kHz expected

#### Configuration Functions

##### `esp_err_t visualizer_set_color_scheme(led_color_scheme_t scheme)`
Change the LED color scheme.

**Parameters:**
- `scheme`: Color scheme selection
  - `LED_COLOR_RAINBOW` - Spectrum of colors
  - `LED_COLOR_HEAT` - Red/yellow/white gradient
  - `LED_COLOR_MONOCHROME` - Single color
  - `LED_COLOR_CUSTOM` - User-defined colors

**Returns:** `ESP_OK` on success

##### `esp_err_t visualizer_set_smoothing(float factor)`
Adjust LED transition smoothing for fluid animations.

**Parameters:**
- `factor`: Smoothing level (0.0 = none, 1.0 = maximum)

**Recommended values:**
- 0.3-0.5: Responsive, good for percussion
- 0.6-0.8: Smooth, good for ambient
- 0.9-1.0: Very smooth, slow transitions

##### `esp_err_t visualizer_set_peak_hold(bool enable)`
Enable or disable peak hold indicators.

**Parameters:**
- `enable`: true to show peak hold, false to disable

**Peak Hold Behavior:**
- Holds peak for 1.5 seconds (configurable)
- Decays at 20dB/second
- Displayed as single bright LED above current level

##### `esp_err_t visualizer_set_gain(float gain)`
Adjust visualization sensitivity.

**Parameters:**
- `gain`: Gain multiplier (0.5 to 2.0 typical)
  - < 1.0: Reduce sensitivity for loud sources
  - 1.0: Normal gain
  - > 1.0: Increase sensitivity for quiet sources

#### Status and Control

##### `esp_err_t visualizer_get_stats(visualizer_stats_t* stats)`
Retrieve operational statistics.

**Parameters:**
- `stats`: Pointer to statistics structure

**Statistics Structure:**
```c
typedef struct {
    uint32_t samples_processed;  // Total audio samples
    uint32_t fft_runs;          // FFT calculations (future)
    uint32_t buffer_level;      // Current buffer usage
    uint32_t buffer_overruns;   // Data loss events
    uint32_t led_updates;       // LED refresh count
    uint32_t led_errors;        // LED driver errors
    bool task_running;          // Task status
} visualizer_stats_t;
```

##### `bool visualizer_is_active(void)`
Check if visualizer is initialized and running.

**Returns:** true if active, false otherwise

##### `esp_err_t visualizer_suspend(void)` / `esp_err_t visualizer_resume(void)`
Temporarily suspend LED updates for flash-safe operations.

**Usage:**
```c
// Suspend before NVS operations
visualizer_suspend();
nvs_set_blob(handle, "config", data, size);
nvs_commit(handle);
visualizer_resume();
```

### PCM Visualizer API

#### Audio Processing Functions

##### `esp_err_t pcm_viz_init(void)`
Initialize the PCM audio analysis module.

**Returns:** `ESP_OK` on success

##### `esp_err_t pcm_viz_deinit(void)`
Shutdown PCM analyzer and free resources.

##### `esp_err_t pcm_viz_write(const uint8_t* data, size_t len)`
Write PCM data to the analysis ring buffer.

**Parameters:**
- `data`: PCM audio data
- `len`: Size in bytes

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if buffer full

##### `esp_err_t pcm_viz_get_loudness(pcm_viz_loudness_t* loudness)`
Get current loudness/PPM measurements.

**Loudness Structure:**
```c
typedef struct {
    float left;              // Left PPM level (0.0-1.0)
    float right;             // Right PPM level (0.0-1.0)
    float left_db;           // Left level in dB
    float right_db;          // Right level in dB
    float left_peak_db;      // Left peak hold dB
    float right_peak_db;     // Right peak hold dB
    uint32_t left_peak_time; // Left peak timestamp
    uint32_t right_peak_time;// Right peak timestamp
    bool left_overload;      // Left clipping flag
    bool right_overload;     // Right clipping flag
    float auto_gain;         // Current auto-gain value
} pcm_viz_loudness_t;
```

##### `void pcm_viz_set_gain(float gain)`
Set audio analysis gain.

**Parameters:**
- `gain`: Gain multiplier (1.0 = unity)

### LED Strip Controller API

#### LED Control Functions

##### `esp_err_t led_controller_init(led_controller_t* controller, const visualizer_led_config_t* config)`
Initialize LED strip hardware.

**Configuration Structure:**
```c
typedef struct {
    uint8_t gpio_pin;        // Data output GPIO
    uint8_t num_leds;        // Number of LEDs (129)
    uint8_t brightness;      // Global brightness (0-255)
    bool enable_dma;         // Use DMA transfers
    led_color_scheme_t color_scheme;
    float smoothing_factor;  // Transition smoothing
} visualizer_led_config_t;
```

##### `esp_err_t led_controller_update_from_loudness(led_controller_t* controller, float left_loudness, float right_loudness)`
Update LED display from loudness values.

**Parameters:**
- `controller`: LED controller handle
- `left_loudness`: Left channel level (0.0-1.0)
- `right_loudness`: Right channel level (0.0-1.0)

##### `esp_err_t led_controller_render(led_controller_t* controller)`
Push frame buffer to physical LEDs.

**Returns:** `ESP_OK` on successful transmission

##### `esp_err_t led_controller_power_on_animation(led_controller_t* controller)`
Play startup animation (red sweep effect).

##### `void led_controller_deinit(led_controller_t* controller)`
Release LED driver resources.

## Configuration Parameters (Kconfig)

### Audio Processing Configuration

```kconfig
# Chunk and Buffer Sizes
CONFIG_VIZ_CHUNK_SIZE=512          # Processing chunk size
CONFIG_VIZ_RING_SIZE=8192          # Ring buffer size
CONFIG_VIZ_WINDOW_SIZE=1024        # RMS window (21.3ms @ 48kHz)

# VU Meter Ballistics (values in millipercent)
CONFIG_VIZ_ATTACK_COEFF_MPCT=950   # Attack coefficient (0.95)
CONFIG_VIZ_RELEASE_COEFF_MPCT=850  # Release coefficient (0.85)

# Peak Hold Settings
CONFIG_VIZ_PEAK_HOLD_MS=1500       # Peak hold time
CONFIG_VIZ_PEAK_DECAY_DB_PER_SEC_TENTHS=200  # 20.0 dB/sec

# Auto-Gain Control (values in millipercent)
CONFIG_VIZ_AUTO_GAIN_MIN_MPCT=500      # Min gain (0.5)
CONFIG_VIZ_AUTO_GAIN_MAX_MPCT=2000     # Max gain (2.0)
CONFIG_VIZ_AUTO_GAIN_TARGET_MPCT=800   # Target level (0.8)
CONFIG_VIZ_AUTO_GAIN_ADJUST_RATE_MPCT=50  # Adjust rate (0.05)
CONFIG_VIZ_AUTO_GAIN_WINDOW_MS=1000    # AGC window

# PPM Meter Configuration (values in tenths)
CONFIG_VIZ_PPM_MIN_DB_TENTHS=-400      # -40.0 dB minimum
CONFIG_VIZ_PPM_MAX_DB_TENTHS=30        # +3.0 dB maximum
CONFIG_VIZ_PPM_DB_RANGE_TENTHS=430     # 43.0 dB range
CONFIG_VIZ_PPM_OVERLOAD_DB_TENTHS=0    # 0.0 dB overload
CONFIG_VIZ_PPM_DECAY_DB_PER_SEC_TENTHS=200  # 20.0 dB/sec

# Smoothing
CONFIG_VIZ_SMOOTH_COEFF_MPCT=500       # Smoothing (0.5)
```

### LED Hardware Configuration

```kconfig
# GPIO Configuration
CONFIG_VIZ_LED_GPIO=21              # Data pin for WS2812B

# LED Physical Layout
CONFIG_VIZ_LED_COUNT=129            # Total LEDs (128 active)
CONFIG_VIZ_LED_COLUMNS=64           # Matrix columns
CONFIG_VIZ_LED_ROWS=2               # Matrix rows

# Performance
CONFIG_VIZ_LED_DMA_ENABLE=y         # Use DMA for LED updates
CONFIG_VIZ_LED_REFRESH_HZ=60        # Target refresh rate
```

## Color Schemes

### Rainbow Mode
Progressive hue shift across the spectrum:
```c
// Hue calculation for rainbow effect
float hue = (column_index / 64.0f) * 360.0f;
rgb_color_t color = hsv_to_rgb(hue, 1.0, brightness);
```

### Heat Mode
Temperature-based gradient (cool → warm):
- 0-25%: Blue → Cyan
- 25-50%: Cyan → Green
- 50-75%: Green → Yellow
- 75-100%: Yellow → Red → White

### Monochrome Mode
Single color with brightness variation:
```c
// Configurable base color
rgb_color_t base = {0, 255, 0};  // Green
rgb_color_t output = {
    base.r * brightness,
    base.g * brightness,
    base.b * brightness
};
```

### Custom Mode
User-defined color mapping function:
```c
// Example: Dual-color gradient
rgb_color_t custom_color(float level, int column) {
    if (column < 32) {  // Left channel
        return blend_colors(BLUE, CYAN, level);
    } else {  // Right channel
        return blend_colors(MAGENTA, YELLOW, level);
    }
}
```

## PPM Meter and Loudness Analysis

### PPM (Peak Programme Meter) Implementation

The visualizer implements a quasi-PPM meter following IEC 60268-10 Type I characteristics:

```
Signal Flow:
PCM Input → RMS Calculation → dB Conversion → PPM Ballistics → LED Mapping

Key Characteristics:
- Integration time: 10ms (fast attack)
- Decay rate: 20dB/second (1.5 sec for 30dB)
- Scale: -40dB to +3dB (43dB range)
- Overload indication: ≥ 0dB
```

### Loudness Calculation Pipeline

1. **RMS Measurement**
   ```c
   // 21.3ms window at 48kHz
   float rms = sqrt(sum_of_squares / window_samples);
   ```

2. **dB Conversion**
   ```c
   float db = 20.0f * log10f(rms / 32768.0f);
   ```

3. **PPM Ballistics**
   ```c
   if (current > previous) {
       // Fast attack
       output = previous + (current - previous) * 0.95f;
   } else {
       // Slow release
       output = previous + (current - previous) * 0.15f;
   }
   ```

4. **Auto-Gain Control**
   ```c
   // Adjust gain to maintain 80% peak utilization
   if (peak < 0.7f) gain *= 1.01f;  // Increase slowly
   if (peak > 0.9f) gain *= 0.99f;  // Decrease slowly
   gain = CLAMP(gain, 0.5f, 2.0f);
   ```

## Example Usage Scenarios

### Audio Receiver Integration
```c
// Integration with RTP audio receiver
void audio_receiver_callback(uint8_t* pcm_data, size_t len) {
    // Process received audio
    audio_output_write(pcm_data, len);
    
    // Send to visualizer
    if (visualizer_is_active()) {
        visualizer_feed_pcm(pcm_data, len);
    }
}
```

### Music Player Visualization
```c
// MP3 decoder with visualization
void mp3_decode_task(void* params) {
    mp3_decoder_t* decoder = mp3_decoder_init();
    
    while (1) {
        int16_t pcm_out[1152 * 2];  // MP3 frame size
        size_t decoded = mp3_decode_frame(decoder, pcm_out);
        
        if (decoded > 0) {
            // Play audio
            i2s_write(I2S_NUM_0, pcm_out, decoded, portMAX_DELAY);
            
            // Visualize
            visualizer_feed_pcm((uint8_t*)pcm_out, decoded);
        }
    }
}
```

### Live Performance Mode
```c
// Dynamic visualization for live audio
typedef struct {
    float sensitivity;
    led_color_scheme_t scheme;
    bool auto_mode;
} performance_config_t;

void setup_performance_mode(performance_config_t* config) {
    visualizer_init();
    
    // High sensitivity for live performance
    visualizer_set_gain(config->sensitivity);
    
    // Fast response
    visualizer_set_smoothing(0.3f);
    
    // Enable peak indicators
    visualizer_set_peak_hold(true);
    
    // Auto color cycling
    if (config->auto_mode) {
        xTaskCreate(color_cycle_task, "color", 2048, NULL, 5, NULL);
    }
}

void color_cycle_task(void* params) {
    led_color_scheme_t schemes[] = {
        LED_COLOR_RAINBOW,
        LED_COLOR_HEAT,
        LED_COLOR_MONOCHROME
    };
    int index = 0;
    
    while (1) {
        visualizer_set_color_scheme(schemes[index]);
        index = (index + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(30000));  // Change every 30 seconds
    }
}
```

### Multi-Zone Installation
```c
// Synchronized multi-zone visualization
void sync_visualizer_network(void) {
    // Master unit broadcasts timing
    if (is_master) {
        while (1) {
            sync_packet_t sync = {
                .timestamp = esp_timer_get_time(),
                .color_scheme = current_scheme,
                .gain = current_gain
            };
            udp_broadcast(&sync, sizeof(sync));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else {
        // Slave units follow master
        while (1) {
            sync_packet_t sync;
            if (udp_receive(&sync, sizeof(sync)) == ESP_OK) {
                visualizer_set_color_scheme(sync.color_scheme);
                visualizer_set_gain(sync.gain);
            }
        }
    }
}
```

## Performance Characteristics

### Resource Usage

| Resource | Usage | Notes |
|----------|-------|-------|
| CPU | ~15-20% | Audio processing + LED updates |
| RAM | ~12KB | Buffers + task stacks |
| Flash | ~25KB | Code + color tables |
| DMA Channels | 1 | RMT for LED data |

### Timing Analysis

```
Audio Processing Pipeline:
├─ PCM Write:        < 1ms   (Ring buffer write)
├─ RMS Calculation:  ~2ms    (1024 samples)
├─ PPM Ballistics:   < 0.5ms (IIR filters)
├─ LED Mapping:      ~1ms    (128 LEDs)
└─ RMT Transmission: ~5ms    (DMA transfer)

Total Latency: ~10ms (from audio input to LED output)
Update Rate: 60 Hz (configurable)
```

### Optimization Techniques

1. **Double Buffering** - Prevents LED flicker during updates
2. **Fixed-Point Math** - Faster calculations where possible
3. **DMA Transfers** - Offload LED data transmission
4. **Circular Buffers** - Lock-free audio data exchange
5. **Task Priorities** - Audio processing at higher priority than LED updates

## Troubleshooting Guide

### Common Issues and Solutions

#### No LED Output
```c
// Diagnostic code
esp_err_t diagnose_leds(void) {
    // Check GPIO configuration
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_VIZ_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    // Test with simple pattern
    led_controller_t test_ctrl;
    visualizer_led_config_t config = {
        .gpio_pin = CONFIG_VIZ_LED_GPIO,
        .num_leds = 129,
        .brightness = 128
    };
    
    ESP_ERROR_CHECK(led_controller_init(&test_ctrl, &config));
    ESP_ERROR_CHECK(led_controller_power_on_animation(&test_ctrl));
    
    return ESP_OK;
}
```

#### LEDs Flicker or Show Wrong Colors
- Check power supply voltage (should be stable 5V)
- Add 1000µF capacitor near LED strip power input
- Verify ground connection between ESP32 and LED strip
- Use level shifter for 3.3V → 5V signal conversion
- Reduce LED brightness if power supply is inadequate

#### Audio Not Affecting LEDs
```c
// Debug audio flow
void debug_audio_path(void) {
    pcm_viz_stats_t stats;
    pcm_viz_get_stats(&stats);
    
    ESP_LOGI(TAG, "Samples processed: %lu", stats.samples_processed);
    ESP_LOGI(TAG, "Buffer level: %lu", stats.buffer_level);
    ESP_LOGI(TAG, "Overruns: %lu", stats.buffer_overruns);
    
    pcm_viz_loudness_t loudness;
    pcm_viz_get_loudness(&loudness);
    
    ESP_LOGI(TAG, "Left: %.2f dB (%.1f%%)", 
             loudness.left_db, loudness.left * 100);
    ESP_LOGI(TAG, "Right: %.2f dB (%.1f%%)", 
             loudness.right_db, loudness.right * 100);
}
```

#### LEDs Stuck at Full Brightness
- Check for buffer overruns causing invalid data
- Verify PCM data format (must be 16-bit signed)
- Ensure [`visualizer_feed_pcm()`](include/visualizer_task.h:47) is called regularly
- Check auto-gain isn't set too high

#### Performance Issues
```c
// Monitor task performance
void monitor_performance(void) {
    visualizer_stats_t stats;
    visualizer_get_stats(&stats);
    
    static uint32_t last_samples = 0;
    static uint32_t last_updates = 0;
    
    uint32_t sample_rate = stats.samples_processed - last_samples;
    uint32_t update_rate = stats.led_updates - last_updates;
    
    ESP_LOGI(TAG, "Sample rate: %lu Hz", sample_rate);
    ESP_LOGI(TAG, "LED update rate: %lu Hz", update_rate);
    ESP_LOGI(TAG, "Buffer overruns: %lu", stats.buffer_overruns);
    
    last_samples = stats.samples_processed;
    last_updates = stats.led_updates;
}
```

### Debug Configuration

Enable detailed logging in menuconfig:
```
Component config → Visualizer → Debug Settings:
[x] Enable debug logging
[x] Log buffer statistics
[x] Log loudness values
[x] Log LED frame timing
```

## Dependencies

```yaml
dependencies:
  espressif/led_strip: "^3.0.1"    # WS2812B LED driver
  espressif/esp-dsp: "^1.3.0"      # DSP functions (future use)
  
idf_components:
  - freertos                       # Task management
  - driver                         # RMT, GPIO drivers
  - esp_timer                      # Timing functions
  - log                           # Logging support
```

## Thread Safety

- [`visualizer_feed_pcm()`](include/visualizer_task.h:47) - Thread-safe, uses ring buffer
- [`visualizer_set_*()`](include/visualizer_task.h) functions - Thread-safe, atomic operations
- [`pcm_viz_write()`](include/pcm_visualizer.h:95) - Thread-safe, lock-free ring buffer
- LED functions - Not thread-safe, call from single task

## Migration Guide

### From Raw LED Control
```c
// Old: Direct LED manipulation
for (int i = 0; i < num_leds; i++) {
    set_led_color(i, calculate_color(audio_level));
}

// New: Visualizer component
//visualizer_init();
//visualizer_feed_pcm(audio_data, audio_size);
// LEDs update automatically!
```

### From FFT-based Visualization
```c
// Old: FFT spectrum analysis
fft_compute(audio_buffer, fft_output);
map_fft_to_leds(fft_output, led_buffer);

// New: PPM-based visualization (lower latency)
//visualizer_init();
//visualizer_set_color_scheme(LED_COLOR_RAINBOW);
//visualizer_feed_pcm(audio_buffer, buffer_size);
```

## Future Enhancements

- **Frequency Spectrum Mode** - FFT-based visualization
- **Beat Detection** - Automatic tempo synchronization
- **Preset System** - Save/load visualization configurations
- **WiFi Control** - Web interface for remote configuration
- **DMX512 Output** - Professional lighting integration
- **Audio Reactive Patterns** - Complex animations triggered by audio events

## Version History

- **1.0.0** - Initial release
  - Three-module architecture
  - PPM metering with auto-gain
  - Multiple color schemes
  - Peak hold visualization
  - Suspend/resume for flash safety

## License

This component is distributed under the Apache License 2.0. See LICENSE file for details.

## Support

For issues, feature requests, or contributions, please visit:
https://github.com/netham45/esp32-visualizer