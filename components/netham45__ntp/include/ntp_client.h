#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

// No mDNS specific includes needed here

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes and starts the NTP client task.
 *
 * This task will periodically query a DNS server via multicast for the target hostname,
 * connect to the resolved IP on the specified NTP port, receive the time,
 * and update the system clock.
 *
 * This function is safe to call multiple times - it will only initialize once.
 */
void initialize_ntp_client();

/**
 * @brief Deinitializes and stops the NTP client task.
 *
 * This function stops the NTP client task and cleans up resources.
 * Safe to call even if NTP client was never initialized.
 */
void deinitialize_ntp_client();

#ifdef __cplusplus
}
#endif

#endif // NTP_CLIENT_H
