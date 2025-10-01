#pragma once
#include "stdint.h"
#include "config.h"
#include "stdbool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// PCM Bytes per chunk, non-configurable (Part of Scream)
#define PCM_CHUNK_SIZE 1152

// Network activity monitoring
#define NETWORK_PACKET_RECEIVED_BIT BIT0
extern EventGroupHandle_t s_network_activity_event_group;

#include "usb/uac_host.h"
// Global USB speaker device handle
extern uac_host_device_handle_t s_spk_dev_handle;
