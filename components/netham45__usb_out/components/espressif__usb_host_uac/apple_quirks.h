#include "uac_quirks.h"

// Apple USB-C to 3.5mm Headphone Jack Adapter config quirk
static const uac_quirk_t apple_dongle_config_quirk = {
    .vid = 0x05AC,
    .pid = 0x110A,
    .type = UAC_QUIRK_FIXED_CONFIG,
    .config_num = 2
};

// Apple USB-C to 3.5mm Headphone Jack Adapter endpoint quirk - Format 1 (24-bit)
static const uac_quirk_t apple_dongle_endpoint_quirk_24bit = {
    .vid = 0x05AC,
    .pid = 0x110A,
    .type = UAC_QUIRK_FIXED_ENDPOINT,
    .endpoint = {
        .iface_num = 1,
        .alt_setting = 0,  // This is the array index, not the actual alt setting value
        .channels = 2,
        .bit_resolution = 24,  // Original value was 24-bit
        .endpoint = 0x02,
        .ep_attr = 0x09, // USB_BM_ATTRIBUTES_SYNC_SYNC
        .rates = {44100, 48000},
        .nr_rates = 2
    }
};

// Apple USB-C to 3.5mm Headphone Jack Adapter endpoint quirk - Format 2 (16-bit)
static const uac_quirk_t apple_dongle_endpoint_quirk_16bit = {
    .vid = 0x05AC,
    .pid = 0x110A,
    .type = UAC_QUIRK_FIXED_ENDPOINT,
    .endpoint = {
        .iface_num = 1,
        .alt_setting = 1,  // This is the array index, not the actual alt setting value
        .channels = 2,
        .bit_resolution = 16,
        .endpoint = 0x02,
        .ep_attr = 0x05, // USB_BM_ATTRIBUTES_SYNC_ADAPTIVE
        .rates = {44100, 48000},
        .nr_rates = 2
    }
};