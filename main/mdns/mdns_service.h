#ifndef MDNS_SERVICE_H
#define MDNS_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize mDNS service
 *
 * Initializes the mDNS system and sets the hostname and instance name.
 * This should be called once during system initialization, after WiFi is initialized.
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t mdns_service_init(void);

/**
 * @brief Start mDNS service advertisement
 *
 * Advertises the _scream._udp service on the network with TXT records
 * containing device information (mode, type, battery, MAC, sample rates, etc.).
 * Also starts periodic TXT record updates.
 *
 * Note: mdns_service_init() must be called first.
 */
void mdns_service_start(void);

/**
 * @brief Stop mDNS service advertisement
 *
 * Removes the _scream._udp service from the network and stops periodic TXT updates.
 */
void mdns_service_stop(void);

/**
 * @brief Deinitialize mDNS service
 *
 * Stops advertisement and frees all mDNS resources.
 * This should be called during system shutdown.
 */
void mdns_service_deinit(void);

/**
 * @brief Update the mDNS hostname without requiring a reboot
 *
 * Stops the current mDNS service, updates the hostname configuration,
 * and restarts the service with the new hostname. This allows hostname
 * changes to take effect immediately without system restart.
 *
 * @return ESP_OK on success, or appropriate error code on failure
 */
esp_err_t mdns_service_update_hostname(void);

/**
 * @brief Check if mDNS service is initialized
 *
 * @return true if initialized, false otherwise
 */
bool mdns_service_is_initialized(void);

/**
 * @brief Check if mDNS service advertisement is active
 *
 * @return true if advertising, false otherwise
 */
bool mdns_service_is_advertising(void);

/**
 * @brief Run pending periodic TXT record updates.
 *
 * Invoking this function replaces the previous background task that refreshed
 * TXT records. Call it regularly from a cooperative loop to keep the
 * advertisement metadata fresh.
 */
void mdns_service_txt_update_tick(void);

#ifdef __cplusplus
}
#endif

#endif // MDNS_SERVICE_H