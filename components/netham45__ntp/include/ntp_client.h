#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes and starts the NTP client task.
 *
 * This task will:
 * - Query mDNS for "screamrouter.local" NTP server
 * - Initialize SNTP for wall-clock time synchronization
 * - Run high-rate NTP micro-probes for precision audio synchronization
 * - Maintain a PLL for offset and skew tracking
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

// ============================================================================
// Precision Time Synchronization API for Audio
// ============================================================================

/**
 * @brief Convert master (NTP) time to local monotonic time
 *
 * This function uses the internal PLL to convert NTP/Unix time (from RTP/RTCP
 * packets) to local monotonic time (esp_timer_get_time()) for precise scheduling.
 *
 * @param master_us Master time in Unix microseconds
 * @return Local monotonic time in microseconds, or -1 if PLL not valid
 *
 * @note Use this to convert RTP presentation timestamps to local timer values
 *       for scheduling audio playback with GPTimer interrupts.
 */
int64_t ntp_master_to_local(int64_t master_us);

/**
 * @brief Convert local monotonic time to master (NTP) time
 *
 * This function uses the internal PLL to convert local monotonic time
 * (esp_timer_get_time()) to NTP/Unix time for sending in RTP/RTCP packets.
 *
 * @param local_mono_us Local monotonic time in microseconds
 * @return Master time in Unix microseconds, or -1 if PLL not valid
 *
 * @note Use this to generate accurate timestamps in outgoing RTCP packets.
 */
int64_t ntp_local_to_master(int64_t local_mono_us);

/**
 * @brief Get current PLL offset and skew
 *
 * Returns the current state of the PLL for monitoring and diagnostics.
 *
 * @param offset_us Output: current offset in microseconds (can be NULL)
 *                  Positive = local clock is ahead of master
 * @param skew_ppm Output: current skew in parts per million (can be NULL)
 *                 Positive = local clock is running fast
 * @return true if PLL is valid and has been initialized, false otherwise
 *
 * @note Call this periodically to monitor sync quality. Typical offset should be
 *       < 1ms, typical skew should be < 100 ppm for good crystal oscillators.
 */
bool ntp_get_pll_state(double *offset_us, double *skew_ppm);

/**
 * @brief Get current PLL convergence status
 *
 * Returns the current convergence status of the PLL including correction history.
 * Useful for monitoring convergence and determining when the clock is stable.
 *
 * @param corrections_applied Output: total number of corrections applied (can be NULL)
 * @param total_correction_us Output: total correction amount in microseconds (can be NULL)
 * @param current_error_us Output: current error in microseconds (can be NULL)
 * @return true if PLL is valid and converged (error < 500µs), false otherwise
 *
 * @note A converged clock typically has error < 500µs. During convergence, multiple
 *       corrections may be applied, especially for offsets in the 500µs - 100ms range.
 */
bool ntp_get_convergence_status(int *corrections_applied, int64_t *total_correction_us, int64_t *current_error_us);

/**
 * @brief Manually trigger an NTP micro-probe burst
 *
 * Immediately performs a burst of NTP queries (8 samples) and updates the PLL
 * with the min-RTT sample. Useful for:
 * - Testing the sync system
 * - Getting a fresh offset measurement before critical audio timing
 * - Recovering from network interruptions
 *
 * @return true if successful and PLL was updated, false otherwise
 *
 * @note This is a blocking call that takes ~100ms to complete (burst + network latency).
 *       The background task already does this periodically (5 Hz initially, then 1 Hz).
 */
/**
 * @brief Dynamically set NTP client configuration
 *
 * When use_mdns is true, the client will mDNS-resolve "screamrouter.local" periodically.
 * When false, the client will resolve and use the provided host and use 'port' for micro-probes.
 * Note: SNTP uses port 123 internally; 'port' applies to precision micro-probes only.
 */
void ntp_client_set_config(bool use_mdns, const char* host, uint16_t port);

bool ntp_trigger_probe();

#ifdef __cplusplus
}
#endif

#endif // NTP_CLIENT_H
