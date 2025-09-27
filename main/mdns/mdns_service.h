#ifndef MDNS_SERVICE_H
#define MDNS_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes and starts the mDNS service.
 *
 * Sets up the mDNS service with the delegate hostname "_sink._screamrouter" and advertises
 * the "_scream._udp" service on port 4010 (Scream data port).
 * Includes TXT records for service type and potentially audio settings later.
 */
void mdns_service_start(void);

/**
 * @brief Stops the mDNS service.
 */
void mdns_service_stop(void);

/**
 * @brief Updates the mDNS hostname without requiring a reboot
 *
 * Stops the current mDNS service, updates the hostname configuration,
 * and restarts the service with the new hostname. This allows hostname
 * changes to take effect immediately without system restart.
 *
 * @return ESP_OK on success, or appropriate error code on failure
 */
esp_err_t mdns_service_update_hostname(void);

#ifdef __cplusplus
}
#endif

#endif // MDNS_SERVICE_H
