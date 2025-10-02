#ifndef MDNS_DISCOVERY_H
#define MDNS_DISCOVERY_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdint.h>
#include <time.h>
#include "build_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DISCOVERED_DEVICES CONFIG_MDNS_MAX_DEVICES

// Holds information for a device discovered via mDNS
typedef struct {
    char hostname[64];
    esp_ip4_addr_t ip_addr;
    uint16_t port;
    time_t last_seen;  // Timestamp when device was last seen
} discovered_device_t;

/**
 * @brief Start the continuous mDNS discovery task
 *
 * Starts a background task that periodically queries the network for
 * _scream._udp services and maintains a list of discovered devices.
 *
 * Note: mdns_service_init() must be called before starting discovery.
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t mdns_discovery_start(void);

/**
 * @brief Stop the continuous mDNS discovery task
 *
 * Stops the discovery task and clears the list of discovered devices.
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t mdns_discovery_stop(void);

/**
 * @brief Get the list of discovered devices
 *
 * Returns a snapshot of the currently discovered devices. Automatically
 * removes stale devices (not seen for 2 minutes) before returning.
 *
 * @param out_devices Array to store discovered devices
 * @param max_devices Maximum number of devices that can be stored
 * @param out_count Pointer to store the actual number of devices found
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t mdns_discovery_get_devices(discovered_device_t *out_devices, size_t max_devices, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // MDNS_DISCOVERY_H