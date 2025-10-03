#include "uac_quirks.h"
#include "esp_log.h"
#include "usb/uac_host.h"
#include "apple_quirks.h"

static const char *TAG = "uac-quirks";

// List of all known quirks
static const uac_quirk_t known_quirks[] = {
    apple_dongle_config_quirk,
    apple_dongle_endpoint_quirk_24bit,
    apple_dongle_endpoint_quirk_16bit,
    // Add more quirks here as needed
};

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
void uac_apply_endpoint_quirk(void *iface_alt_ptr, const uac_quirk_t *quirk, uint8_t alt_idx)
{
    if (!iface_alt_ptr || !quirk || quirk->type != UAC_QUIRK_FIXED_ENDPOINT) {
        return;
    }
    
    ESP_LOGI(TAG, "Applying fixed endpoint parameters for VID=0x%04x, PID=0x%04x, alt_setting=%d",
             quirk->vid, quirk->pid, alt_idx);
    
    uac_iface_alt_t *iface_alt = (uac_iface_alt_t *)iface_alt_ptr;
    
    // Apply the fixed endpoint parameters
    iface_alt->dev_alt_param.channels = quirk->endpoint.channels;
    iface_alt->dev_alt_param.bit_resolution = quirk->endpoint.bit_resolution;
    iface_alt->dev_alt_param.sample_freq_type = quirk->endpoint.nr_rates;
    
    // Copy the sample rates
    for (int i = 0; i < quirk->endpoint.nr_rates && i < 8; i++) {
        iface_alt->dev_alt_param.sample_freq[i] = quirk->endpoint.rates[i];
    }
    
    iface_alt->ep_addr = quirk->endpoint.endpoint;
    iface_alt->ep_attr = quirk->endpoint.ep_attr;
    
    // Set feature unit and volume/mute channel map for the Apple adapter
    iface_alt->feature_unit = 2; // Feature unit ID
    iface_alt->vol_ch_map = 0x03; // Enable volume control for both channels
    iface_alt->mute_ch_map = 0x03; // Enable mute control for both channels
    
    ESP_LOGI(TAG, "Successfully applied fixed endpoint parameters for alt_setting=%d",
             alt_idx);
}

bool uac_check_quirks(const usb_device_desc_t *dev_desc, uac_quirk_t *quirk)
{
    if (!dev_desc || !quirk) {
        return false;
    }

    // Loop through all known quirks once
    for (int i = 0; i < sizeof(known_quirks) / sizeof(known_quirks[0]); i++) {
        // Check if this quirk matches the device
        if (dev_desc->idVendor == known_quirks[i].vid &&
            dev_desc->idProduct == known_quirks[i].pid) {
            
            // If we're looking for a specific quirk type
            if (quirk->type != UAC_QUIRK_NONE) {
                // If this quirk matches the requested type
                if (quirk->type == known_quirks[i].type) {
                    ESP_LOGI(TAG, "Found quirk type %d for device VID=0x%04x, PID=0x%04x",
                            known_quirks[i].type, known_quirks[i].vid, known_quirks[i].pid);
                    
                    *quirk = known_quirks[i];
                    return true;
                }
                // Continue searching for the requested type
                continue;
            } else {
                // If no specific type was requested, return this quirk
                ESP_LOGI(TAG, "Found quirk type %d for device VID=0x%04x, PID=0x%04x",
                        known_quirks[i].type, known_quirks[i].vid, known_quirks[i].pid);
                
                *quirk = known_quirks[i];
                return true;
            }
        }
    }

    // No matching quirk found
    return false;
}

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
bool uac_check_endpoint_quirk(const usb_device_desc_t *dev_desc, uint8_t iface_num, uint8_t alt_setting, uac_quirk_t *quirk)
{
    if (!dev_desc || !quirk) {
        return false;
    }

    // Loop through all known quirks
    for (int i = 0; i < sizeof(known_quirks) / sizeof(known_quirks[0]); i++) {
        // Check if this quirk matches the device, type, interface, and alternate setting
        if (dev_desc->idVendor == known_quirks[i].vid &&
            dev_desc->idProduct == known_quirks[i].pid &&
            known_quirks[i].type == UAC_QUIRK_FIXED_ENDPOINT &&
            known_quirks[i].endpoint.iface_num == iface_num &&
            known_quirks[i].endpoint.alt_setting == alt_setting) {
            
            ESP_LOGI(TAG, "Found endpoint quirk for device VID=0x%04x, PID=0x%04x, interface %d, alt_setting %d",
                    known_quirks[i].vid, known_quirks[i].pid, iface_num, alt_setting);
            
            *quirk = known_quirks[i];
            return true;
        }
    }

    // No matching quirk found
    return false;
}