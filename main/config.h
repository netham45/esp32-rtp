#pragma once
#include "build_config.h"  // Kconfig-backed build-time options

// TCP port for Scream server data (from Kconfig)
#define PORT CONFIG_RTP_PORT

// Number of chunks to be buffered before playback starts, configurable
#define INITIAL_BUFFER_SIZE 4
// Number of chunks to add each underflow, configurable
#define BUFFER_GROW_STEP_SIZE 2
// Max number of chunks to be buffered before packets are dropped, configurable
#define  MAX_BUFFER_SIZE 24
// Max number of chunks to be targeted for buffer
#define MAX_GROW_SIZE 16

// Sample rate for incoming PCM (from Kconfig)
#define SAMPLE_RATE CONFIG_SAMPLE_RATE
// Bit depth for incoming PCM (from Kconfig)
#define BIT_DEPTH CONFIG_BIT_DEPTH
// Volume 0.0f-1.0f (from Kconfig percent)
#define VOLUME (CONFIG_DEFAULT_VOLUME_PCT / 100.0f)

// Time to wake from deep sleep to check for DAC (in ms)
#define DAC_CHECK_SLEEP_TIME_MS 2000

// Sleep on silence configuration
#define SILENCE_THRESHOLD_MS 30000       // Sleep after 10 seconds of silence
#define NETWORK_CHECK_INTERVAL_MS 1000   // Check network every 1 second during light sleep
#define ACTIVITY_THRESHOLD_PACKETS 2     // Resume on detecting 2 or more packets
#define SILENCE_AMPLITUDE_THRESHOLD 10   // Audio amplitude below this is considered silent (0-32767)
#define NETWORK_INACTIVITY_TIMEOUT_MS 5000 // Enter sleep mode after no packets for 5 seconds


#define TAG "scream_receiver"
