#pragma once
#include "build_config.h"
#include "stdint.h"
#include "config.h"
#include "stdbool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// PCM bytes per chunk (from Kconfig to keep single source of truth)
#define PCM_CHUNK_SIZE CONFIG_PCM_CHUNK_SIZE

// Network activity monitoring
#define NETWORK_PACKET_RECEIVED_BIT BIT0
extern EventGroupHandle_t s_network_activity_event_group;

#include "usb/uac_host.h"
// Global USB speaker device handle
extern uac_host_device_handle_t s_spk_dev_handle;
