# ESP32 S/PDIF Output (I2S-based)

High-performance S/PDIF transmitter implemented with ESP-IDF I2S peripheral using bi-phase mark (BMC) encoding. Accepts 16-bit stereo PCM data and outputs consumer S/PDIF digital audio signal with dynamic sample rate support.

## Overview

This component implements a complete S/PDIF transmitter using the ESP32's I2S peripheral. It encodes PCM audio data into S/PDIF format using bi-phase mark encoding and transmits it through a single GPIO pin. The implementation supports dynamic sample rate changes without requiring full reinitialization.

**Repository:** https://github.com/netham45/esp32-spdif-out  
**Version:** 1.0.0  
**Supported targets:** esp32, esp32s2, esp32s3, esp32p4  
**ESP-IDF:** >= 5.0

## Key Features

- **I2S-based S/PDIF transmission** using bi-phase mark encoding
- **Dynamic sample rate support** - change rates on the fly
- **16-bit stereo PCM input** format
- **Double buffering** for smooth, uninterrupted audio output
- **DMA-driven transmission** for efficient CPU usage
- **Consumer S/PDIF format** compatible with standard audio equipment
- **APLL clock source** for accurate sample rates

## Data Flow

```mermaid
flowchart LR
  APP[Application PCM data] --> WRITE[spdif_write()]
  WRITE --> BMC[BMC Encoder]
  BMC --> BUF[Double Buffer]
  BUF --> I2S[I2S DMA]
  I2S --> GPIO[GPIO Output Pin]
  GPIO --> SPDIF[S/PDIF Signal]
```

## Hardware Requirements

### Output Circuit Options

#### 1. Optical Transmitter (TOSLINK)
Connect an optical transmitter module (e.g., TOTX173, DLT1111) to the configured GPIO:
- GPIO → Transmitter VIN (through 100Ω resistor)
- 3.3V → Transmitter VCC
- GND → Transmitter GND

#### 2. Coaxial Output
For coaxial S/PDIF output, use a proper output circuit:
- GPIO → 470Ω resistor → Transformer primary
- Transformer secondary → RCA connector
- Use a 1:1 pulse transformer (e.g., PE-65612, DA101C)

#### 3. Direct Connection (Testing Only)
For testing with equipment that accepts 3.3V logic levels:
- GPIO → 100Ω resistor → S/PDIF input
- Connect grounds between devices

**Note:** Never connect the GPIO directly to consumer audio equipment without proper isolation/level conversion.

## Quick Start

### Basic Initialization and Usage

```c
#include "spdif_out.h"

// 1. Initialize S/PDIF with sample rate and output pin
esp_err_t err = spdif_init(48000, GPIO_NUM_21);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init S/PDIF: %s", esp_err_to_name(err));
    return err;
}

// 2. Start the transmitter
err = spdif_start();
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start S/PDIF: %s", esp_err_to_name(err));
    return err;
}

// 3. Send PCM audio data
int16_t pcm_data[1024]; // Interleaved stereo: L0,R0,L1,R1,...
// ... fill pcm_data with audio samples ...
spdif_write(pcm_data, sizeof(pcm_data));

// 4. Change sample rate dynamically if needed
err = spdif_set_sample_rates(44100);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to change sample rate: %s", esp_err_to_name(err));
}

// 5. Stop and cleanup when done
spdif_stop();   // Stop transmitter but keep driver
spdif_deinit();  // Release all resources
```

## API Reference

### Core Functions

#### `esp_err_t spdif_init(int rate, int pin)`
Initialize the S/PDIF driver with specified sample rate and output pin.

**Parameters:**
- `rate`: Sample rate in Hz (e.g., 44100, 48000, 88200, 96000)
- `pin`: GPIO pin number for S/PDIF output

**Returns:** 
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if rate is invalid
- `ESP_ERR_INVALID_STATE` if already initialized
- Error code from I2S driver on failure

**Example:**
```c
esp_err_t err = spdif_init(48000, GPIO_NUM_21);
```

#### `esp_err_t spdif_start(void)`
Start the S/PDIF transmitter. Must be called after [`spdif_init()`](include/spdif_out.h:13).

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized
- Error code from I2S driver on failure

#### `esp_err_t spdif_stop(void)`
Stop the S/PDIF transmitter but keep the driver installed. Can be restarted with [`spdif_start()`](include/spdif_out.h:19).

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized
- Error code from I2S driver on failure

#### `esp_err_t spdif_deinit(void)`
Release all S/PDIF driver resources. Automatically stops transmitter if running.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized
- Error code from I2S driver on failure

#### `void spdif_write(const void *src, size_t size)`
Send PCM audio data to the S/PDIF transmitter.

**Parameters:**
- `src`: Pointer to 16-bit stereo PCM data (interleaved format)
- `size`: Number of bytes to send (must be even)

**Notes:**
- Data must be 16-bit signed integers in little-endian format
- Stereo interleaved: [L0,R0,L1,R1,...]
- Size must be even (full samples only)
- Function blocks until data is written to I2S buffer

**Example:**
```c
int16_t stereo_samples[512]; // 256 stereo frames
// ... fill with audio data ...
spdif_write(stereo_samples, sizeof(stereo_samples));
```

#### `esp_err_t spdif_set_sample_rates(int rate)`
Change the sampling rate dynamically. Temporarily stops and restarts the transmitter if running.

**Parameters:**
- `rate`: New sample rate in Hz

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized
- Error code on failure

**Example:**
```c
// Switch from 48kHz to 44.1kHz
esp_err_t err = spdif_set_sample_rates(44100);
```

## Supported Sample Rates

The driver supports standard audio sample rates:
- **44100 Hz** - CD quality
- **48000 Hz** - DAT/DVD quality
- **88200 Hz** - 2x CD quality
- **96000 Hz** - High-resolution audio
- **176400 Hz** - 4x CD quality
- **192000 Hz** - Studio quality

Other rates are possible but may have reduced clock accuracy depending on the ESP32's APLL capabilities.

## PCM Data Format

### Input Format Specification
- **Bit depth:** 16-bit signed integers (int16_t)
- **Channels:** 2 (stereo)
- **Byte order:** Little-endian
- **Interleaving:** Interleaved stereo [L0,R0,L1,R1,...]
- **Alignment:** Data pointer must be 2-byte aligned

### Buffer Requirements
- Minimum write size: 4 bytes (1 stereo sample)
- Recommended buffer size: 2048-4096 bytes for smooth playback
- Maximum write size: Limited by available heap/stack

### Example Data Preparation
```c
// Generate 1kHz test tone at 48kHz sample rate
#define SAMPLE_RATE 48000
#define FREQUENCY 1000
#define AMPLITUDE 16000

int16_t test_buffer[SAMPLE_RATE]; // 0.5 seconds stereo
for (int i = 0; i < SAMPLE_RATE/2; i++) {
    float sample = AMPLITUDE * sinf(2.0f * M_PI * FREQUENCY * i / SAMPLE_RATE);
    test_buffer[i*2] = (int16_t)sample;     // Left channel
    test_buffer[i*2+1] = (int16_t)sample;   // Right channel
}

spdif_write(test_buffer, sizeof(test_buffer));
```

## Usage Scenarios

### Digital Audio Output to DAC/Amplifier
```c
// Stream audio to external DAC
esp_err_t setup_dac_output(void) {
    ESP_ERROR_CHECK(spdif_init(48000, GPIO_NUM_21));
    ESP_ERROR_CHECK(spdif_start());
    
    // Configure your audio pipeline to send PCM to spdif_write()
    return ESP_OK;
}
```

### Home Theater Connection
```c
// Output to AV receiver with automatic sample rate matching
void output_to_receiver(int16_t *audio_data, size_t size, int sample_rate) {
    static int current_rate = 0;
    
    if (current_rate != sample_rate) {
        ESP_ERROR_CHECK(spdif_set_sample_rates(sample_rate));
        current_rate = sample_rate;
    }
    
    spdif_write(audio_data, size);
}
```

### Audio Streaming Bridge
```c
// Bridge network audio to S/PDIF output
void audio_bridge_task(void *pvParameters) {
    int16_t *pcm_buffer = malloc(4096);
    
    while (1) {
        size_t bytes_received = receive_network_audio(pcm_buffer, 4096);
        if (bytes_received > 0) {
            spdif_write(pcm_buffer, bytes_received);
        }
    }
}
```

## Hardware Connection Examples

### Optical Output Circuit
```
     ESP32 GPIO
         |
      [100Ω]
         |
    TOTX173/DLT1111
    +-----+-----+
    | VIN | VCC |---- 3.3V
    |     |     |
    | GND | OUT |---- Optical Fiber
    +-----+-----+
         |
        GND
```

### Coaxial Output Circuit
```
     ESP32 GPIO              Pulse Transformer
         |                    (1:1, PE-65612)
      [470Ω]                Primary   Secondary
         |                     ___||___
         +--------------------[   ||   ]---- RCA Center
                              [   ||   ]
        GND ------------------[___||___]---- RCA Shield
```

### Professional Output (Balanced)
```
     ESP32 GPIO          Transformer       XLR Connector
         |               (1:1.4 ratio)
      [100Ω]              ___||___
         +---------------[   ||   ]-------- Pin 2 (Hot)
                        [   ||   ]
                        [   ||   ]-------- Pin 3 (Cold)
        GND ------------[___||___]
                             |
                           Pin 1 (Shield)
```

## Configuration Constants

The following constants in [`spdif_out.c`](spdif_out.c) can be modified if needed:

- **`I2S_NUM`** (0): I2S peripheral number
- **`DMA_BUF_COUNT`** (2): Number of DMA buffers
- **`DMA_BUF_LEN`** (384): DMA buffer length in samples
- **`SPDIF_BLOCK_SAMPLES`** (192): S/PDIF block size
- **`I2S_BUG_MAGIC`** (26000000): Clock workaround value

## Performance Characteristics

### Timing and Latency
- **Output latency:** < 10ms typical
- **Buffer size:** 192 samples per S/PDIF block
- **DMA efficiency:** Double buffering prevents underruns
- **Clock accuracy:** APLL provides < 50ppm frequency error

### Resource Usage
- **CPU usage:** < 5% for continuous streaming
- **RAM usage:** ~3KB for buffers and driver
- **DMA channels:** 1 I2S TX channel
- **Interrupts:** I2S DMA completion interrupts

### Limitations
- Single S/PDIF output channel only
- No support for compressed formats (AC3, DTS)
- Channel status bits set to default consumer format
- No HDMI ARC compatibility
- Maximum 24-bit audio (truncated to 16-bit internally)

## Troubleshooting Guide

### No Output Signal
- Verify GPIO pin supports I2S output on your ESP32 variant
- Check output circuit connections and power supply
- Ensure [`spdif_start()`](include/spdif_out.h:19) was called after init
- Verify PCM data is being sent via [`spdif_write()`](include/spdif_out.h:38)

### Audio Distortion
- Confirm sample rate matches source material
- Check for buffer underruns (write data continuously)
- Verify PCM data format (16-bit signed, little-endian)
- Ensure proper grounding between devices

### Receiver Not Locking
- Add proper output circuit (resistor/transformer)
- Verify S/PDIF signal levels (0.5-0.6Vpp for consumer)
- Check cable quality and length (< 10m for optical)
- Try different sample rates

### Clicks or Pops
- Implement fade-in/fade-out when starting/stopping
- Ensure continuous data flow without gaps
- Check for DC offset in PCM data
- Use [`spdif_stop()`](include/spdif_out.h:24) before [`spdif_deinit()`](include/spdif_out.h:31)

### Sample Rate Issues
```c
// Debug sample rate changes
ESP_LOGI(TAG, "Changing rate from %d to %d", old_rate, new_rate);
esp_err_t err = spdif_set_sample_rates(new_rate);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Rate change failed: %s", esp_err_to_name(err));
    // Fallback to previous rate
    spdif_set_sample_rates(old_rate);
}
```

## Dependencies

- **ESP-IDF Components:**
  - driver/i2s
  - freertos
  - esp_log
  
- **Hardware Requirements:**
  - I2S-capable GPIO pin
  - External circuit for proper S/PDIF signal levels

## Thread Safety

- [`spdif_write()`](include/spdif_out.h:38) is thread-safe and can be called from any task
- Init/deinit functions should be called from a single task
- Sample rate changes should be synchronized with audio pipeline

## Example Applications

1. **Wireless Audio Transmitter** - Stream audio from Bluetooth/WiFi to S/PDIF
2. **USB to S/PDIF Converter** - Bridge USB audio to digital output
3. **Multi-room Audio** - Synchronize S/PDIF outputs across multiple ESP32s
4. **Audio Test Generator** - Generate test tones and patterns
5. **Format Converter** - Convert between different digital audio formats

## Version History

- **1.0.0** - Initial release with core S/PDIF output functionality
  - I2S-based BMC encoding
  - Dynamic sample rate support
  - 16-bit PCM input format