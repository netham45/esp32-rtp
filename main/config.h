#pragma once
// TCP port for Scream server data, configurable
#define PORT 4010

// Number of chunks to be buffered before playback starts, configurable
#define INITIAL_BUFFER_SIZE 4
// Number of chunks to add each underflow, configurable
#define BUFFER_GROW_STEP_SIZE 2
// Max number of chunks to be buffered before packets are dropped, configurable
#define  MAX_BUFFER_SIZE 24
// Max number of chunks to be targeted for buffer
#define MAX_GROW_SIZE 16

// Sample rate for incoming PCM, configurable
#define SAMPLE_RATE 48000
// Bit depth for incoming PCM, non-configurable (does not implement 24->32 bit padding)
#define BIT_DEPTH 16
//Volume 0.0f-1.0f
#define VOLUME 1.0f

// Time to wake from deep sleep to check for DAC (in ms)
#define DAC_CHECK_SLEEP_TIME_MS 2000

// Sleep on silence configuration
#define SILENCE_THRESHOLD_MS 30000       // Sleep after 10 seconds of silence
#define NETWORK_CHECK_INTERVAL_MS 1000   // Check network every 1 second during light sleep
#define ACTIVITY_THRESHOLD_PACKETS 2     // Resume on detecting 2 or more packets
#define SILENCE_AMPLITUDE_THRESHOLD 10   // Audio amplitude below this is considered silent (0-32767)
#define NETWORK_INACTIVITY_TIMEOUT_MS 5000 // Enter sleep mode after no packets for 5 seconds


#define IS_SPDIF
#define IS_USB

#define TAG "scream_receiver"
