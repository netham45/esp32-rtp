#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "usb/usb_types_ch9.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UAC quirk types
 */
typedef enum {
    UAC_QUIRK_NONE = 0,
    UAC_QUIRK_FIXED_CONFIG = 1,       // Force a specific configuration
    UAC_QUIRK_FIXED_ENDPOINT = 2,     // Use fixed endpoint parameters
} uac_quirk_type_t;

/**
 * @brief UAC fixed endpoint parameters
 */
typedef struct {
    uint8_t iface_num;                // Interface number
    uint8_t alt_setting;              // Alternate setting
    uint8_t channels;                 // Number of channels
    uint8_t bit_resolution;           // Bit resolution
    uint8_t endpoint;                 // Endpoint address
    uint8_t ep_attr;                  // Endpoint attributes
    uint32_t rates[2];                // Supported sample rates
    uint8_t nr_rates;                 // Number of supported rates
} uac_fixed_endpoint_t;

/**
 * @brief UAC device quirk
 */
typedef struct {
    uint16_t vid;                     // Vendor ID
    uint16_t pid;                     // Product ID
    uac_quirk_type_t type;            // Quirk type
    union {
        uint8_t config_num;           // Configuration number for UAC_QUIRK_FIXED_CONFIG
        uac_fixed_endpoint_t endpoint; // Endpoint parameters for UAC_QUIRK_FIXED_ENDPOINT
    };
} uac_quirk_t;

/**
 * @brief Check if a device has a quirk
 *
 * @param dev_desc USB device descriptor
 * @param quirk Pointer to store the quirk if found
 * @return true if a quirk was found, false otherwise
 */
bool uac_check_quirks(const usb_device_desc_t *dev_desc, uac_quirk_t *quirk);

/**
 * @brief Check for endpoint quirks with specific parameters
 *
 * This function checks if there are any endpoint quirks for the specified device,
 * interface, and alternate setting.
 *
 * @param dev_desc USB device descriptor
 * @param iface_num Interface number
 * @param alt_setting Alternate setting index (0-based)
 * @param quirk Pointer to store the quirk if found
 * @return true if a matching quirk was found, false otherwise
 */
bool uac_check_endpoint_quirk(const usb_device_desc_t *dev_desc, uint8_t iface_num, uint8_t alt_setting, uac_quirk_t *quirk);

/**
 * @brief Apply fixed endpoint parameters for a device
 *
 * This function applies endpoint parameters from a quirk to an interface alternate setting.
 * The caller must provide the following fields in the structure pointed to by iface_alt_ptr:
 * - uint8_t alt_idx: The alternate setting index
 * - uint8_t ep_addr: The endpoint address
 * - uint8_t ep_attr: The endpoint attributes
 * - uint8_t feature_unit: The feature unit ID
 * - uint8_t vol_ch_map: The volume channel map
 * - uint8_t mute_ch_map: The mute channel map
 * - struct { uint8_t channels, bit_resolution, sample_freq_type; uint32_t sample_freq[8]; } dev_alt_param
 *
 * @param iface_alt_ptr Pointer to interface alternate setting structure
 * @param quirk Quirk to apply
 * @param alt_idx The alternate setting index to check against
 */
void uac_apply_endpoint_quirk(void *iface_alt_ptr, const uac_quirk_t *quirk, uint8_t alt_idx);

#ifdef __cplusplus
}
#endif