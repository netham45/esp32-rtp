#ifndef DEVICE_DISCOVERY_H
#define DEVICE_DISCOVERY_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DISCOVERED_DEVICES 16

// Holds information for a device discovered via a raw DNS query.
typedef struct {
    char hostname[64];
    esp_ip4_addr_t ip_addr;
    uint16_t port;
    time_t last_seen;  // Timestamp when device was last seen
} discovered_device_t;

/**
 * @brief Start the continuous mDNS discovery task
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t mdns_discovery_start(void);

/**
 * @brief Stop the continuous mDNS discovery task
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t mdns_discovery_stop(void);

/**
 * @brief Get the list of discovered devices
 * @param out_devices Array to store discovered devices
 * @param max_devices Maximum number of devices that can be stored
 * @param out_count Pointer to store the actual number of devices found
 * @return ESP_OK on success, or an error code on failure
 */

/**
 * @brief Enable or disable mDNS service advertisement
 * 
 * @param enable true to enable advertisement, false to disable
 */
void mdns_discovery_enable_advertisement(bool enable);
esp_err_t mdns_discovery_get_devices(discovered_device_t *out_devices, size_t max_devices, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // DEVICE_DISCOVERY_H