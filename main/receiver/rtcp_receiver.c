#include "rtcp_receiver.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <arpa/inet.h>
#include <math.h>   // for isfinite() check on slope
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// SR freshness threshold (ms). Use Kconfig if defined; default to 15000 ms.
#ifndef CONFIG_RTCP_SR_MAX_AGE_MS
#define CONFIG_RTCP_SR_MAX_AGE_MS 15000
#endif

#ifndef CONFIG_RTCP_PLL_KB
#define CONFIG_RTCP_PLL_KB 0.02
#endif
#ifndef CONFIG_RTCP_PLL_KI
#define CONFIG_RTCP_PLL_KI 0.00005
#endif
#ifndef CONFIG_RTCP_PLL_KA
#define CONFIG_RTCP_PLL_KA 1.0e-9
#endif
#ifndef CONFIG_RTCP_PLL_APPLY_INTERVAL_MS
#define CONFIG_RTCP_PLL_APPLY_INTERVAL_MS 20
#endif
#ifndef CONFIG_RTCP_PLL_SLOPE_PPM_LIMIT
#define CONFIG_RTCP_PLL_SLOPE_PPM_LIMIT 300
#endif
#ifndef CONFIG_RTCP_PLL_OFFSET_STEP_LIMIT_US
#define CONFIG_RTCP_PLL_OFFSET_STEP_LIMIT_US 200
#endif

// Outlier/step detection thresholds (ms); compile-time defaults if Kconfig not present
#ifndef CONFIG_RTCP_OUTLIER_MAX_EARLY_MS
#define CONFIG_RTCP_OUTLIER_MAX_EARLY_MS 500   // Reject packets that map >500 ms too early
#endif
#ifndef CONFIG_RTCP_OUTLIER_MAX_LATE_MS
#define CONFIG_RTCP_OUTLIER_MAX_LATE_MS 200    // Reject packets that map >200 ms too late
#endif
#ifndef CONFIG_RTCP_SR_OFFSET_STEP_MS
#define CONFIG_RTCP_SR_OFFSET_STEP_MS 100      // SR-derived offset step detection threshold
#endif
#ifndef CONFIG_RTCP_SR_RESEED_HOLDOFF_MS
#define CONFIG_RTCP_SR_RESEED_HOLDOFF_MS 200   // Min time between reseeds to avoid thrash
#endif
#ifndef CONFIG_RTCP_PLL_OBS_OUTLIER_FACTOR
#define CONFIG_RTCP_PLL_OBS_OUTLIER_FACTOR 6   // PLL observation outlier threshold (x sample window)
#endif

// Low-rate summary intervals (defaults if missing from Kconfig)
#ifndef CONFIG_RTCP_LOG_SUMMARY_INTERVAL_MS
#define CONFIG_RTCP_LOG_SUMMARY_INTERVAL_MS 5000
#endif
#ifndef CONFIG_RTP_RX_LOG_SUMMARY_INTERVAL_MS
#define CONFIG_RTP_RX_LOG_SUMMARY_INTERVAL_MS 5000
#endif
#ifndef CONFIG_AUDIO_OUT_LOG_SUMMARY_INTERVAL_MS
#define CONFIG_AUDIO_OUT_LOG_SUMMARY_INTERVAL_MS 10000
#endif

// Multi-SSRC hygiene defaults (compile-time)
#ifndef CONFIG_RTCP_SSRC_STALE_MS
#define CONFIG_RTCP_SSRC_STALE_MS 30000        // inactivity threshold for eviction (30s)
#endif
#ifndef CONFIG_RTCP_SSRC_EVICT_ON_MAX
#define CONFIG_RTCP_SSRC_EVICT_ON_MAX 1        // enable eviction when table full
#endif
#ifndef CONFIG_RTCP_PRIMARY_SWITCH_MIN_GAP_MS
#define CONFIG_RTCP_PRIMARY_SWITCH_MIN_GAP_MS 2000   // min inactivity gap to consider switching primary
#endif
#ifndef CONFIG_RTCP_PRIMARY_REQUIRE_FRESH_MS
#define CONFIG_RTCP_PRIMARY_REQUIRE_FRESH_MS 5000    // candidate must be active within this window
#endif

#ifdef CONFIG_RTCP_ENABLED

static const char *TAG = "rtcp_receiver";

// RTP unwrap/internal constants (local to this file)
#define RTP_WRAP_THRESHOLD (0x80000000u / 2)    // half-range to disambiguate wrap
#define RTP_REORDER_TOL_TICKS (CONFIG_SAMPLE_RATE / 10) // ~100ms worth of RTP ticks at 48kHz

// Optional detailed logging for unwrapping
#ifdef CONFIG_RTCP_LOG_UNWRAP
#define UNWRAP_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define UNWRAP_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define UNWRAP_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#else
#define UNWRAP_LOGI(...)
#define UNWRAP_LOGW(...)
#define UNWRAP_LOGD(...)
#endif

// RX stats logging thresholds and heuristics
#ifndef RX_SEQ_JUMP_WARN
#define RX_SEQ_JUMP_WARN 10  // Warn if sequence advances by >10 packets at once
#endif
#ifndef RX_JITTER_WARN_TICKS
// Approximate threshold: 3x a nominal packet duration (~6ms at 48kHz => 18ms)
// This keeps logging conservative without depending on PCM_CHUNK_SIZE here.
#define RX_JITTER_WARN_TICKS ((uint32_t)((CONFIG_SAMPLE_RATE * 18) / 1000))
#endif

// RTCP receiver state
static rtcp_state_t rtcp_state;
static SemaphoreHandle_t rtcp_mutex = NULL;

// Forward declaration: low-rate RTCP structured summary
static void rtcp_log_summary_if_due(void);

// Helper: Get current system time as Unix microseconds (µs since 1970-01-01 00:00:00 UTC)
static inline uint64_t get_system_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000000ULL) + (uint64_t)tv.tv_usec;
}

// Evict stale/non-pinned entries and optionally force-evict least-recently-active when full.
// Must be called under rtcp_mutex.
static void rtcp_evict_stale_locked(uint64_t now_mono_us) {
    const uint64_t stale_us = ((uint64_t)CONFIG_RTCP_SSRC_STALE_MS) * 1000ULL;

    // First pass: evict stale entries
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        rtcp_sync_info_t *s = &rtcp_state.sync_info[i];
        if (s->valid && !s->preferred_pin) {
            uint64_t last = s->last_activity_mono_us;
            uint64_t age_us = (last == 0ULL || now_mono_us < last) ? UINT64_MAX : (now_mono_us - last);
            if (age_us > stale_us) {
#ifdef CONFIG_RTCP_LOG_SSRC
                uint64_t age_ms = (age_us == UINT64_MAX) ? 0ULL : (age_us / 1000ULL);
                ESP_LOGW(TAG, "Evict stale SSRC 0x%08X age=%llu ms", s->ssrc, (unsigned long long)age_ms);
#endif
                s->valid = false;
                if (rtcp_state.active_sources > 0) {
                    rtcp_state.active_sources--;
                }
            }
        }
    }

    // If still full, optionally force-evict least-recently-active non-pinned
    if (rtcp_state.active_sources >= RTCP_MAX_SSRC_SOURCES) {
#if CONFIG_RTCP_SSRC_EVICT_ON_MAX
        int victim = -1;
        uint64_t victim_last = UINT64_MAX; // choose the smallest last_activity (oldest)
        for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
            rtcp_sync_info_t *s = &rtcp_state.sync_info[i];
            if (s->valid && !s->preferred_pin) {
                if (s->last_activity_mono_us < victim_last) {
                    victim_last = s->last_activity_mono_us;
                    victim = i;
                }
            }
        }
        if (victim >= 0) {
#ifdef CONFIG_RTCP_LOG_SSRC
            uint64_t age_us = (rtcp_state.sync_info[victim].last_activity_mono_us == 0ULL || now_mono_us < rtcp_state.sync_info[victim].last_activity_mono_us)
                                ? UINT64_MAX
                                : (now_mono_us - rtcp_state.sync_info[victim].last_activity_mono_us);
            uint64_t age_ms = (age_us == UINT64_MAX) ? 0ULL : (age_us / 1000ULL);
            ESP_LOGW(TAG, "Forced eviction (full): SSRC 0x%08X age=%llu ms",
                     rtcp_state.sync_info[victim].ssrc, (unsigned long long)age_ms);
#endif
            rtcp_state.sync_info[victim].valid = false;
            if (rtcp_state.active_sources > 0) {
                rtcp_state.active_sources--;
            }
        }
#endif
    }
}

// Initialize RTCP receiver
esp_err_t rtcp_init(void) {
    ESP_LOGI(TAG, "Initializing RTCP receiver");
    
    // Create mutex for thread-safe access
    if (rtcp_mutex == NULL) {
        rtcp_mutex = xSemaphoreCreateMutex();
        if (rtcp_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create RTCP mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Initialize state
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    memset(&rtcp_state, 0, sizeof(rtcp_state));
    rtcp_state.initialized = true;
    xSemaphoreGive(rtcp_mutex);
    
    ESP_LOGI(TAG, "RTCP receiver initialized (max %d sources)", RTCP_MAX_SSRC_SOURCES);
    return ESP_OK;
}

// Find or allocate sync info for an SSRC
static rtcp_sync_info_t* find_or_allocate_sync_info(uint32_t ssrc) {
    // First, look for existing entry
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            return &rtcp_state.sync_info[i];
        }
    }

    // Try to allocate a free slot
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (!rtcp_state.sync_info[i].valid) {
            memset(&rtcp_state.sync_info[i], 0, sizeof(rtcp_sync_info_t));
            rtcp_state.sync_info[i].ssrc = ssrc;
            rtcp_state.sync_info[i].valid = true;
            rtcp_state.active_sources++;

            // Initialize PLL accumulators for this SSRC
            rtcp_state.sync_info[i].pll_offset_b_us        = (double)rtcp_state.sync_info[i].offset_b_mono_us;
            rtcp_state.sync_info[i].pll_slope_a_correction = 0.0;
            rtcp_state.sync_info[i].pll_i_err              = 0.0;
            rtcp_state.sync_info[i].pll_obs_count          = 0;
            rtcp_state.sync_info[i].pll_last_apply_mono    = 0;

#ifdef CONFIG_RTCP_LOG_SSRC
            ESP_LOGI(TAG, "Allocated sync slot %d for SSRC 0x%08X", i, ssrc);
#endif
            return &rtcp_state.sync_info[i];
        }
    }

    // No free slots: attempt stale eviction (and optional forced eviction if enabled)
    uint64_t now = esp_timer_get_time();
    rtcp_evict_stale_locked(now);

    // Retry allocation after eviction
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (!rtcp_state.sync_info[i].valid) {
            memset(&rtcp_state.sync_info[i], 0, sizeof(rtcp_sync_info_t));
            rtcp_state.sync_info[i].ssrc = ssrc;
            rtcp_state.sync_info[i].valid = true;
            rtcp_state.active_sources++;

            // Initialize PLL accumulators for this SSRC
            rtcp_state.sync_info[i].pll_offset_b_us        = (double)rtcp_state.sync_info[i].offset_b_mono_us;
            rtcp_state.sync_info[i].pll_slope_a_correction = 0.0;
            rtcp_state.sync_info[i].pll_i_err              = 0.0;
            rtcp_state.sync_info[i].pll_obs_count          = 0;
            rtcp_state.sync_info[i].pll_last_apply_mono    = 0;

#ifdef CONFIG_RTCP_LOG_SSRC
            ESP_LOGI(TAG, "Allocated sync slot (post-evict) for SSRC 0x%08X", ssrc);
#endif
            return &rtcp_state.sync_info[i];
        }
    }

    // Still full; respect eviction policy (e.g., all slots may be pinned). Return NULL.
#ifdef CONFIG_RTCP_LOG_SSRC
    ESP_LOGW(TAG, "No free sync slot for SSRC 0x%08X (all slots in use or pinned)", ssrc);
#endif
    return NULL;
}

// Internal helper: reseed mapping and reset PLL under rtcp_mutex
static void rtcp_reseed_mapping_locked(rtcp_sync_info_t* s, int64_t new_b, bool reset_slope) {
    if (!s) return;
    const double a0 = 1000000.0 / (double)CONFIG_SAMPLE_RATE;
    // Set new offset b (mono = ntp + b)
    s->offset_b_mono_us = new_b;

    // Optionally reset slope back to nominal if requested
    if (reset_slope) {
        s->slope_a_us_per_tick = a0;
    }

    // Reset PLL accumulators
    s->pll_offset_b_us        = (double)new_b;
    s->pll_slope_a_correction = (s->slope_a_us_per_tick / a0) - 1.0;
    s->pll_i_err              = 0.0;
    s->pll_obs_count          = 0;
    s->pll_last_apply_mono    = 0;
}
 
// Parse RTCP packet and update synchronization info
esp_err_t rtcp_parse_packet(const uint8_t *packet, size_t len) {
    if (!packet || len < sizeof(rtcp_header_t)) {
        ESP_LOGW(TAG, "Invalid RTCP packet: too small (%d bytes)", len);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!rtcp_state.initialized) {
        ESP_LOGE(TAG, "RTCP not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0;
    bool parsed_any = false;

    // Iterate over each RTCP sub-packet in a compound RTCP packet
    while (offset + sizeof(rtcp_header_t) <= len) {
        const uint8_t *ptr = packet + offset;
        const rtcp_header_t *header = (const rtcp_header_t *)ptr;

        // Validate RTCP version per sub-packet
        uint8_t version = RTCP_VERSION(header->vprc);
        if (version != RTCP_VERSION_NUM) {
            ESP_LOGW(TAG, "Invalid RTCP version: %d (expected %d) at offset %u",
                     version, RTCP_VERSION_NUM, (unsigned)offset);
            return ESP_ERR_INVALID_ARG;
        }

        // Compute this sub-packet's size ((length + 1) * 4 bytes)
        uint8_t packet_type = header->pt;
        uint16_t length_words = ntohs(header->length);
        size_t packet_size = ((size_t)length_words + 1U) * 4U;

        // Validate sub-packet size and bounds
        if (packet_size < sizeof(rtcp_header_t) || offset + packet_size > len) {
            ESP_LOGW(TAG, "RTCP sub-packet size invalid: len_words=%u -> bytes=%u, remaining=%u",
                     (unsigned)length_words, (unsigned)packet_size, (unsigned)(len - offset));
            return ESP_ERR_INVALID_SIZE;
        }

        switch (packet_type) {
            case RTCP_SR: {
                // Sender Report: must contain header + 6 words after SSRC (NTP sec/frac, RTP ts, counts)
                if (packet_size < sizeof(rtcp_sr_packet_t)) {
                    ESP_LOGW(TAG, "SR packet too small: %u bytes", (unsigned)packet_size);
                    return ESP_ERR_INVALID_SIZE;
                }

                const rtcp_sr_packet_t *sr = (const rtcp_sr_packet_t *)ptr;
                uint32_t ssrc = ntohl(sr->ssrc);
                uint32_t ntp_sec = ntohl(sr->ntp_sec);
                uint32_t ntp_frac = ntohl(sr->ntp_frac);
                uint32_t rtp_ts = ntohl(sr->rtp_timestamp);

                // Convert sender's NTP to microseconds and capture receiver's system time (NTP-synced)
                uint64_t sender_ntp_us = ((uint64_t)(ntp_sec - NTP_EPOCH_OFFSET) * 1000000ULL) +
                                         NTP_FRAC_TO_USEC(ntp_frac);
                uint64_t receiver_ntp_us = get_system_time_us();
                uint64_t mono_now = esp_timer_get_time();  // Still needed for staleness tracking

                // Obtain unwrapped RTP timestamp for SR without holding RTCP mutex
                uint64_t sr_rtp64 = 0;
                if (rtcp_unwrap_rtp_timestamp(ssrc, rtp_ts, &sr_rtp64) != ESP_OK) {
                    // Fallback: seed with 32-bit value if unwrap fails (rare)
                    sr_rtp64 = (uint64_t)rtp_ts;
                }

                // Update per-SSRC sync state and seed linear mapping
                xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
                rtcp_sync_info_t *sync_info = find_or_allocate_sync_info(ssrc);
                if (sync_info) {
                    // Preferred fields for future mapping
                    sync_info->last_sr_mono_us = mono_now;
                    sync_info->last_sr_ntp_us  = sender_ntp_us;
                    sync_info->last_sr_rtp32   = rtp_ts;

                    // Store raw NTP sec/frac for LSR computation
                    sync_info->last_sr_ntp_sec  = ntp_sec;
                    sync_info->last_sr_ntp_frac = ntp_frac;

                    // Compute offset between receiver and sender NTP clocks (should be small, ±microseconds to milliseconds)
                    int64_t new_b = (int64_t)receiver_ntp_us - (int64_t)sender_ntp_us;
                    bool seeded = (sync_info->rtp_sr_base64 != 0 && sync_info->ntp_sr_base_us != 0 && sync_info->mono_sr_base_us != 0);

                    if (seeded) {
                        int64_t delta_b = new_b - sync_info->offset_b_mono_us;
                        if (delta_b < 0) delta_b = -delta_b;
                        if ((uint64_t)delta_b > ((uint64_t)CONFIG_RTCP_SR_OFFSET_STEP_MS * 1000ULL)) {
                            uint64_t last_apply = sync_info->pll_last_apply_mono;
                            uint64_t since = (mono_now >= last_apply) ? (mono_now - last_apply) : 0ULL;
                            if (since > ((uint64_t)CONFIG_RTCP_SR_RESEED_HOLDOFF_MS * 1000ULL)) {
                                // Treat as clock step and reseed mapping and PLL
                                const double a0 = 1000000.0 / (double)CONFIG_SAMPLE_RATE;
                                double ppm_dev = fabs((sync_info->slope_a_us_per_tick / a0) - 1.0) * 1.0e6;
                                bool reset_slope = (ppm_dev > 50.0);
                                rtcp_reseed_mapping_locked(sync_info, new_b, reset_slope);
#ifdef CONFIG_RTCP_LOG_SYNC_INFO
                                static uint32_t reseed_log_counter = 0;
                                if ((++reseed_log_counter % 50) == 0) {
                                    ESP_LOGW(TAG, "RTCP reseed due to SR offset step: delta_b=%lldus", (long long)delta_b);
                                }
#endif
                            }
                        }
                    } else {
                        // First SR: seed mapping to nominal slope and observed offset
                        rtcp_reseed_mapping_locked(sync_info, new_b, true);

                        // Establish stable NTP→monotonic baseline for multi-speaker sync
                        if (!sync_info->ntp_to_mono_baseline_valid) {
                            sync_info->ntp_to_mono_baseline_ntp_us = receiver_ntp_us;
                            sync_info->ntp_to_mono_baseline_mono_us = mono_now;
                            sync_info->ntp_to_mono_baseline_valid = true;
                            ESP_LOGI(TAG, "SSRC 0x%08X: Established NTP→mono baseline at NTP=%llu mono=%llu",
                                     ssrc,
                                     (unsigned long long)receiver_ntp_us,
                                     (unsigned long long)mono_now);
                        }
                    }

                    // Update SR bases for RTP->time mapping
                    sync_info->rtp_sr_base64   = sr_rtp64;
                    sync_info->mono_sr_base_us = mono_now;
                    sync_info->ntp_sr_base_us  = sender_ntp_us;

                    // Keep legacy fields updated for backward compatibility
                    sync_info->ntp_timestamp       = sender_ntp_us;
                    sync_info->rtp_timestamp       = rtp_ts;
                    sync_info->local_time_received = mono_now;

                    // Mark activity on SR receipt
                    sync_info->last_activity_mono_us = mono_now;

                    // Opportunistic stale eviction (decimated)
                    static uint32_t sr_evict_decim = 0;
                    if (rtcp_state.active_sources >= RTCP_MAX_SSRC_SOURCES || ((++sr_evict_decim & 0xFFu) == 0)) {
                        rtcp_evict_stale_locked(mono_now);
                    }
                }
                xSemaphoreGive(rtcp_mutex);

#ifdef CONFIG_RTCP_LOG_SYNC_INFO
                ESP_LOGI(TAG, "SR from SSRC 0x%08X: sender_NTP=%llu.%06llu, receiver_NTP=%llu.%06llu, offset_b=%lld us, RTP=%u",
                         ssrc,
                         (unsigned long long)(sender_ntp_us / 1000000ULL),
                         (unsigned long long)(sender_ntp_us % 1000000ULL),
                         (unsigned long long)(receiver_ntp_us / 1000000ULL),
                         (unsigned long long)(receiver_ntp_us % 1000000ULL),
                         (long long)((int64_t)receiver_ntp_us - (int64_t)sender_ntp_us),
                         rtp_ts);
#endif
                parsed_any = true;
                break;
            }

            case RTCP_RR: {
                // Receiver Report - acknowledged but not used
                ESP_LOGD(TAG, "Received RR packet (ignored)");
                parsed_any = true;
                break;
            }

            case RTCP_SDES: {
                // Source Description - safe to ignore for sync
                ESP_LOGD(TAG, "Received SDES packet (ignored)");
                parsed_any = true;
                break;
            }

            case RTCP_BYE: {
                // Goodbye - one or more SSRCs leaving
                uint8_t sc = RTCP_RC(header->vprc);
                size_t min_ssrc_bytes = (size_t)sc * 4U;
                if (packet_size < sizeof(rtcp_header_t) + min_ssrc_bytes) {
                    ESP_LOGW(TAG, "BYE packet too small: %u bytes for %u SSRC(s)",
                             (unsigned)packet_size, (unsigned)sc);
                    return ESP_ERR_INVALID_SIZE;
                }

                const uint8_t *ssrc_list = ptr + sizeof(rtcp_header_t);
                xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
                for (uint8_t i = 0; i < sc; i++) {
                    uint32_t ssrc_n;
                    memcpy(&ssrc_n, ssrc_list + (i * 4U), sizeof(uint32_t));
                    uint32_t bye_ssrc = ntohl(ssrc_n);
                    ESP_LOGI(TAG, "BYE from SSRC 0x%08X", bye_ssrc);

                    for (int j = 0; j < RTCP_MAX_SSRC_SOURCES; j++) {
                        if (rtcp_state.sync_info[j].valid &&
                            rtcp_state.sync_info[j].ssrc == bye_ssrc) {
                            rtcp_state.sync_info[j].valid = false;
                            if (rtcp_state.active_sources > 0) {
                                rtcp_state.active_sources--;
                            }
                            // If this was the primary, demote and force re-selection later
                            if (rtcp_state.primary_valid && rtcp_state.primary_ssrc == bye_ssrc) {
                                rtcp_state.primary_valid = false;
#ifdef CONFIG_RTCP_LOG_SSRC
                                ESP_LOGI(TAG, "Primary SSRC 0x%08X cleared due to BYE", bye_ssrc);
#endif
                            }
                            break;
                        }
                    }
                }
                xSemaphoreGive(rtcp_mutex);

                parsed_any = true;
                break;
            }

            default: {
                // Unknown/APP/other - safely skip
                ESP_LOGD(TAG, "Received RTCP packet type %d (skipped)", packet_type);
                break;
            }
        }

        // Advance to next sub-packet
        offset += packet_size;
    }

    // If there are leftover bytes that can't form a header, treat as malformed
    if (offset != len) {
        ESP_LOGW(TAG, "Trailing bytes in RTCP compound packet: %u", (unsigned)(len - offset));
        return ESP_ERR_INVALID_SIZE;
    }

    // Return success if we parsed the compound packet (even if some sub-packets were ignored)
    rtcp_log_summary_if_due();
    return ESP_OK;
}

// Calculate playout time for an RTP packet using per-SSRC linear mapping (RTP ticks -> NTP us -> mono us)
esp_err_t rtcp_calculate_playout_time(uint32_t ssrc, uint32_t rtp_timestamp, uint64_t *playout_time) {
    if (!playout_time) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!rtcp_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 1) Unwrap RTP timestamp without holding RTCP mutex (unwrap locks internally)
    uint64_t rtp64 = 0;
    if (rtcp_unwrap_rtp_timestamp(ssrc, rtp_timestamp, &rtp64) != ESP_OK) {
        // No stable unwrap or state yet; signal caller to fallback
        return ESP_ERR_NOT_FOUND;
    }

    // 2) Snapshot mapping fields under mutex
    double   slope_a_us_per_tick = 0.0;
    int64_t  offset_b_mono_us    = 0;
    uint64_t rtp_sr_base64       = 0;
    uint64_t ntp_sr_base_us      = 0;
    uint64_t mono_sr_base_us     = 0;
    uint64_t baseline_ntp_us     = 0;
    uint64_t baseline_mono_us    = 0;
    bool     baseline_valid      = false;

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    rtcp_sync_info_t *sync_info = NULL;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            sync_info = &rtcp_state.sync_info[i];
            break;
        }
    }
    if (sync_info) {
        slope_a_us_per_tick = sync_info->slope_a_us_per_tick;
        offset_b_mono_us    = sync_info->offset_b_mono_us;
        rtp_sr_base64       = sync_info->rtp_sr_base64;
        ntp_sr_base_us      = sync_info->ntp_sr_base_us;
        mono_sr_base_us     = sync_info->mono_sr_base_us;
        baseline_ntp_us     = sync_info->ntp_to_mono_baseline_ntp_us;
        baseline_mono_us    = sync_info->ntp_to_mono_baseline_mono_us;
        baseline_valid      = sync_info->ntp_to_mono_baseline_valid;
    }
    xSemaphoreGive(rtcp_mutex);

    // 3) Enforce SR freshness/validity before using the mapping
    uint64_t now_mono_us = esp_timer_get_time();
    uint64_t sr_age_us = (mono_sr_base_us > 0 && now_mono_us >= mono_sr_base_us)
                           ? (now_mono_us - mono_sr_base_us)
                           : UINT64_MAX;
    uint64_t max_age_us = (uint64_t)CONFIG_RTCP_SR_MAX_AGE_MS * 1000ULL;
    bool seeded = (rtp_sr_base64 != 0 && ntp_sr_base_us != 0 && mono_sr_base_us != 0);
    if (!seeded || slope_a_us_per_tick <= 0.0 || !isfinite(slope_a_us_per_tick) || sr_age_us > max_age_us) {
#ifdef CONFIG_RTCP_LOG_SYNC_INFO
        static uint32_t stale_log_counter = 0;
        if ((++stale_log_counter % 200) == 0) {
            uint64_t age_ms = (sr_age_us == UINT64_MAX) ? 0ULL : (sr_age_us / 1000ULL);
            ESP_LOGW(TAG, "Sync stale/invalid: SSRC=0x%08X age=%llu ms seeded=%d slope=%f",
                     ssrc,
                     (unsigned long long)age_ms,
                     (int)seeded,
                     slope_a_us_per_tick);
        }
#endif
        return ESP_ERR_NOT_FOUND;
    }

    // 3) Map RTP ticks -> sender NTP time -> receiver NTP time -> monotonic time
    // Step 1: Calculate packet time in sender's NTP domain
    int64_t delta_ticks = (int64_t)rtp64 - (int64_t)rtp_sr_base64;
    double packet_sender_ntp_d = (double)ntp_sr_base_us + (double)delta_ticks * slope_a_us_per_tick;
    int64_t packet_sender_ntp_us = (int64_t)packet_sender_ntp_d;

    // Step 2: Convert to receiver's NTP domain using offset_b (offset_b = receiver_ntp - sender_ntp)
    int64_t packet_receiver_ntp_us = packet_sender_ntp_us + offset_b_mono_us;

    // Step 3: Convert from receiver NTP time to receiver monotonic time using stable baseline
    if (!baseline_valid) {
        ESP_LOGW(TAG, "NTP→mono baseline not yet established for SSRC 0x%08X", ssrc);
        return ESP_ERR_NOT_FOUND;
    }

    // packet_mono = baseline_mono + (packet_receiver_ntp - baseline_ntp)
    // This ensures all speakers use the same NTP→monotonic reference point established at RTCP lock
    int64_t ntp_delta_from_baseline = (int64_t)packet_receiver_ntp_us - (int64_t)baseline_ntp_us;
    int64_t packet_mono_us = (int64_t)baseline_mono_us + ntp_delta_from_baseline;

    uint64_t target_latency_us = (uint64_t)CONFIG_RTCP_TARGET_LATENCY_MS * 1000ULL;
    int64_t playout_i64 = packet_mono_us + (int64_t)target_latency_us;
    if (playout_i64 < 0) {
        playout_i64 = 0;
    }
    *playout_time = (uint64_t)playout_i64;

    // Outlier early/late rejection relative to now
    int64_t ahead_us = (int64_t)(*playout_time) - (int64_t)now_mono_us;
    if (ahead_us > (int64_t)CONFIG_RTCP_OUTLIER_MAX_EARLY_MS * 1000LL) {
#ifdef CONFIG_RTCP_LOG_SYNC_INFO
        static uint32_t early_log_counter = 0;
        if ((++early_log_counter % 200) == 0) {
            ESP_LOGW(TAG, "RTCP playout reject (too early): ahead=%lld ms thr=%d ms",
                     (long long)(ahead_us / 1000LL), (int)CONFIG_RTCP_OUTLIER_MAX_EARLY_MS);
        }
#endif
        return ESP_ERR_NOT_FOUND;
    }
    if ((-ahead_us) > (int64_t)CONFIG_RTCP_OUTLIER_MAX_LATE_MS * 1000LL) {
#ifdef CONFIG_RTCP_LOG_SYNC_INFO
        static uint32_t late_log_counter = 0;
        if ((++late_log_counter % 200) == 0) {
            ESP_LOGW(TAG, "RTCP playout reject (too late): behind=%lld ms thr=%d ms",
                     (long long)((-ahead_us) / 1000LL), (int)CONFIG_RTCP_OUTLIER_MAX_LATE_MS);
        }
#endif
        return ESP_ERR_NOT_FOUND;
    }

#ifdef CONFIG_RTCP_LOG_SYNC_INFO
    static uint32_t log_counter = 0;
    if (++log_counter % 100 == 0) {
        ESP_LOGI(TAG, "Playout SSRC=0x%08X dt=%lld ticks sender_ntp=%lld rcvr_ntp=%lld mono=%lld out=%llu",
                 ssrc,
                 (long long)delta_ticks,
                 (long long)packet_sender_ntp_us,
                 (long long)packet_receiver_ntp_us,
                 (long long)packet_mono_us,
                 (unsigned long long)*playout_time);
    }
#endif

    return ESP_OK;
}

// Generate RTCP Receiver Report (single report block for specified SSRC)
esp_err_t rtcp_generate_rr(uint32_t report_ssrc, uint8_t *buffer, size_t buffer_size, size_t *packet_size) {
    if (!buffer || !packet_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtcp_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t need = sizeof(rtcp_rr_packet_t) + sizeof(rtcp_report_block_t);
    if (buffer_size < need) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Snapshot per-SSRC stats under mutex and update RR interval bases atomically
    uint32_t ext_max_seq = 0;
    uint32_t seq_base = 0;
    uint32_t received_pkts = 0;
    int32_t  cumulative_lost = 0;
    double   jitter_ts = 0.0;
    uint64_t last_sr_mono_us = 0;
    uint32_t last_sr_ntp_sec = 0;
    uint32_t last_sr_ntp_frac = 0;
    uint32_t rr_prev_expected = 0;
    uint32_t rr_prev_received = 0;
    bool seq_initialized = false;

    uint64_t now_us = esp_timer_get_time();

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    rtcp_sync_info_t *sync = NULL;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == report_ssrc) {
            sync = &rtcp_state.sync_info[i];
            break;
        }
    }

    if (!sync || !sync->seq_initialized) {
        xSemaphoreGive(rtcp_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // Snapshot fields for computation outside the lock
    ext_max_seq       = sync->ext_max_seq;
    seq_base          = sync->seq_base;
    received_pkts     = sync->received_pkts;
    cumulative_lost   = sync->cumulative_lost;
    jitter_ts         = sync->jitter_ts;
    last_sr_mono_us   = sync->last_sr_mono_us;
    last_sr_ntp_sec   = sync->last_sr_ntp_sec;
    last_sr_ntp_frac  = sync->last_sr_ntp_frac;
    rr_prev_expected  = sync->rr_prev_expected;
    rr_prev_received  = sync->rr_prev_received;
    seq_initialized   = sync->seq_initialized;

    // Compute current "expected" and interval deltas (RFC 3550)
    uint32_t expected = (ext_max_seq - seq_base) + 1U;
    uint32_t expected_interval = expected - rr_prev_expected;
    uint32_t received_interval = received_pkts - rr_prev_received;
    int32_t  lost_interval = (int32_t)expected_interval - (int32_t)received_interval;

    // Fraction lost over the last interval (8-bit fixed point, [0,255])
    uint8_t fraction_lost = 0;
    if (expected_interval > 0U) {
        int32_t lost_for_frac = (lost_interval < 0) ? 0 : lost_interval;
        uint32_t frac = (uint32_t)(((int64_t)lost_for_frac * 256LL) / (int64_t)expected_interval);
        if (frac > 255U) frac = 255U;
        fraction_lost = (uint8_t)frac;
    }

    // LSR/DLSR
    uint32_t lsr = 0;
    uint32_t dlsr = 0;
    if (last_sr_mono_us != 0 && last_sr_ntp_sec != 0) {
        lsr = ((last_sr_ntp_sec & 0xFFFF) << 16) | ((last_sr_ntp_frac >> 16) & 0xFFFF);
        if (now_us >= last_sr_mono_us) {
            uint64_t delay_us = now_us - last_sr_mono_us;
            double d = ((double)delay_us / 1000000.0) * 65536.0;
            uint64_t d_scaled = (uint64_t)(d + 0.5);
            if (d_scaled > 0xFFFFFFFFULL) d_scaled = 0xFFFFFFFFULL;
            dlsr = (uint32_t)d_scaled;
        } else {
            dlsr = 0;
        }
    }

    // Update interval bases for next RR
    sync->rr_prev_expected = expected;
    sync->rr_prev_received = received_pkts;
    sync->rr_prev_mono_us  = now_us;
    xSemaphoreGive(rtcp_mutex);

    // Store last computed fraction lost for summary
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == report_ssrc) {
            rtcp_state.sync_info[i].rr_prev_fraction_lost = (uint32_t)fraction_lost;
            break;
        }
    }
    xSemaphoreGive(rtcp_mutex);

    // Build RR packet with one report block
    memset(buffer, 0, need);
    rtcp_rr_packet_t *rr = (rtcp_rr_packet_t *)buffer;
    rtcp_report_block_t *rb = (rtcp_report_block_t *)(buffer + sizeof(rtcp_rr_packet_t));

    // Header: Version=2, Padding=0, RC=1
    rr->header.vprc  = (uint8_t)((RTCP_VERSION_NUM << 6) | 0x01);
    rr->header.pt    = RTCP_RR;
    rr->header.length = htons((uint16_t)(((uint16_t)(need / 4U)) - 1U));

    // Local receiver SSRC (placeholder until a real one is wired)
    rr->ssrc = htonl(0x12345678);

    // Report block for the sender SSRC we are reporting on
    rb->ssrc = htonl(report_ssrc);
    rb->fraction_lost = fraction_lost;

    // Cumulative lost (24-bit signed, big-endian; clamp to [-8388608, 8388607])
    const int32_t CUM_MIN = -8388608;
    const int32_t CUM_MAX =  8388607;
    int32_t cum_lost_clamped = cumulative_lost;
    if (cum_lost_clamped < CUM_MIN) cum_lost_clamped = CUM_MIN;
    if (cum_lost_clamped > CUM_MAX) cum_lost_clamped = CUM_MAX;
    uint32_t cum24 = ((uint32_t)cum_lost_clamped) & 0x00FFFFFFu;
    rb->cumulative_lost[0] = (uint8_t)((cum24 >> 16) & 0xFF);
    rb->cumulative_lost[1] = (uint8_t)((cum24 >> 8)  & 0xFF);
    rb->cumulative_lost[2] = (uint8_t)(cum24 & 0xFF);

    // Extended highest seq and jitter
    rb->highest_seq = htonl(ext_max_seq);
    uint32_t jitter_u32 = (jitter_ts <= 0.0) ? 0u :
                          (jitter_ts >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)(jitter_ts));
    rb->jitter = htonl(jitter_u32);

    // LSR/DLSR
    rb->lsr  = htonl(lsr);
    rb->dlsr = htonl(dlsr);

#ifdef CONFIG_RTCP_LOG_RR
    ESP_LOGI(TAG, "RR: src=0x%08X fl=%u cum=%ld ext=0x%08X jit=%u lsr=0x%08X dlsr=0x%08X exp_i=%u rcv_i=%u",
             report_ssrc,
             (unsigned)fraction_lost,
             (long)cum_lost_clamped,
             (unsigned)ext_max_seq,
             (unsigned)jitter_u32,
             (unsigned)lsr,
             (unsigned)dlsr,
             (unsigned)expected_interval,
             (unsigned)received_interval);
#endif

    *packet_size = need;
    return ESP_OK;
}

// Update RTP statistics for a source
void rtcp_update_rtp_stats(uint32_t ssrc, uint16_t seq_num) {
    if (!rtcp_state.initialized) {
        return;
    }
    
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    
    rtcp_sync_info_t *sync_info = NULL;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            sync_info = &rtcp_state.sync_info[i];
            break;
        }
    }
    
    if (sync_info) {
        sync_info->packet_count++;
        
        // Track packet loss
        if (sync_info->packet_count > 1) {
            uint16_t expected_seq = (sync_info->last_seq + 1) & 0xFFFF;
            if (seq_num != expected_seq) {
                int lost = (seq_num - expected_seq) & 0xFFFF;
                if (lost < 1000) {  // Reasonable threshold
                    sync_info->packets_lost += lost;
                }
            }
        }
        sync_info->last_seq = seq_num;
    }
    
    xSemaphoreGive(rtcp_mutex);
}

// Update receiver-side stats (RFC 3550): extended seq, cumulative lost, and interarrival jitter
void rtcp_update_rx_stats(uint32_t ssrc, uint16_t seq, uint32_t rtp_ts, uint32_t arrival_rtp_ticks) {
    if (!rtcp_state.initialized) {
        return;
    }

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);

    uint64_t now_mono = esp_timer_get_time();
    static uint32_t rx_evict_decim = 0;
    if (rtcp_state.active_sources >= RTCP_MAX_SSRC_SOURCES || ((++rx_evict_decim & 0xFFu) == 0)) {
        rtcp_evict_stale_locked(now_mono);
    }

    rtcp_sync_info_t *sync = find_or_allocate_sync_info(ssrc);
    if (!sync) {
        xSemaphoreGive(rtcp_mutex);
        return;
    }
    // Mark activity on any RTP packet observe
    sync->last_activity_mono_us = now_mono;

    bool wrapped = false;
    bool large_jump = false;

    // Sequence extension per RFC 3550
    if (!sync->seq_initialized) {
        sync->seq_base       = (uint32_t)seq;
        sync->max_seq        = (uint32_t)seq;
        sync->cycles         = 0;
        sync->ext_max_seq    = (uint32_t)seq;
        sync->seq_initialized = true;
    } else {
        uint16_t max16   = (uint16_t)sync->max_seq;
        uint16_t udelta  = (uint16_t)(seq - max16); // modulo-16bit difference

        if (udelta < 0x8000u) {
            // Forward movement or small jump
            if (seq < max16) {
                // 16-bit wrap
                sync->cycles += (1u << 16);
                wrapped = true;
            }
            // Large jump detection (heuristic)
            if (udelta > RX_SEQ_JUMP_WARN) {
                large_jump = true;
            }
            sync->max_seq = (uint32_t)seq;
            sync->ext_max_seq = (sync->cycles | (uint32_t)((uint16_t)sync->max_seq));
        }
        // Else: very large backward jump; ignore for ext_max_seq update
    }

    // Packet counters and cumulative loss from receiver view
    sync->received_pkts++;
    if (sync->seq_initialized) {
        uint32_t expected = (sync->ext_max_seq - sync->seq_base) + 1u;
        sync->cumulative_lost = (int32_t)expected - (int32_t)sync->received_pkts;
    }

    // Interarrival jitter (RFC 3550 A.8) - in RTP tick units
    int32_t transit = (int32_t)((int64_t)arrival_rtp_ticks - (int64_t)rtp_ts);
    if (sync->received_pkts > 1) {
        int32_t d = transit - (int32_t)sync->transit_prev;
        if (d < 0) d = -d;
        // J = J + (|D(i-1,i)| - J) / 16
        sync->jitter_ts += ((double)d - sync->jitter_ts) / 16.0;
    }
    sync->transit_prev = (uint32_t)transit;

#ifdef CONFIG_RTCP_LOG_RX_STATS
    if (wrapped) {
        ESP_LOGI(TAG, "SSRC 0x%08X seq wrap: cycles=0x%08X ext_max_seq=0x%08X", ssrc, sync->cycles, sync->ext_max_seq);
    }
    if (large_jump) {
        ESP_LOGW(TAG, "SSRC 0x%08X large seq jump: max_seq=%u -> %u", ssrc, (uint16_t)(sync->max_seq), seq);
    }
    if (sync->jitter_ts > (double)RX_JITTER_WARN_TICKS) {
        ESP_LOGW(TAG, "SSRC 0x%08X high jitter: J=%.2f ticks (thr=%u)", ssrc, sync->jitter_ts, (unsigned)RX_JITTER_WARN_TICKS);
    }
#endif

    xSemaphoreGive(rtcp_mutex);
    rtcp_log_summary_if_due();
}

// Accessors for RX stats snapshot (for RR generation later)
bool rtcp_get_rx_stats(uint32_t ssrc, uint32_t *ext_max_seq, int32_t *cumulative_lost, double *jitter_ts) {
    if (!rtcp_state.initialized) {
        return false;
    }

    bool found = false;

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            rtcp_sync_info_t *sync = &rtcp_state.sync_info[i];
            if (ext_max_seq)      *ext_max_seq = sync->ext_max_seq;
            if (cumulative_lost)  *cumulative_lost = sync->cumulative_lost;
            if (jitter_ts)        *jitter_ts = sync->jitter_ts;
            found = true;
            break;
        }
    }
    xSemaphoreGive(rtcp_mutex);

    return found;
}
 
// Get synchronization info for a source
rtcp_sync_info_t* rtcp_get_sync_info(uint32_t ssrc) {
    if (!rtcp_state.initialized) {
        return NULL;
    }
    
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            return &rtcp_state.sync_info[i];
        }
    }
    
    return NULL;
}

// Check if RTCP timing is available for a source
bool rtcp_has_timing_info(uint32_t ssrc) {
    if (!rtcp_state.initialized) {
        return false;
    }
    
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    
    bool has_timing = false;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && 
            rtcp_state.sync_info[i].ssrc == ssrc &&
            rtcp_state.sync_info[i].rtp_timestamp != 0) {
            has_timing = true;
            break;
        }
    }
    
    xSemaphoreGive(rtcp_mutex);
    return has_timing;
}

/**
* Unwrap a 32-bit RTP timestamp into a stable 64-bit timeline per SSRC.
* Thread-safe: holds RTCP mutex while accessing/updating per-SSRC state.
*/
esp_err_t rtcp_unwrap_rtp_timestamp(uint32_t ssrc, uint32_t rtp32, uint64_t *rtp64_out) {
   if (!rtp64_out) {
       return ESP_ERR_INVALID_ARG;
   }
   if (!rtcp_state.initialized) {
       return ESP_ERR_INVALID_STATE;
   }

   xSemaphoreTake(rtcp_mutex, portMAX_DELAY);

   // Reuse existing allocation pattern under mutex
   rtcp_sync_info_t *sync = find_or_allocate_sync_info(ssrc);
   if (!sync) {
       xSemaphoreGive(rtcp_mutex);
       return ESP_ERR_NO_MEM;
   }

   uint64_t candidate64 = 0;

   // First-time initialization of unwrap state
   if (!sync->unwrap_initialized) {
       if (sync->last_sr_rtp32 != 0) {
           // Initialize baseline from last SR RTP timestamp
           sync->unwrap_cycles   = 0;
           sync->unwrap_last32   = sync->last_sr_rtp32;
           sync->unwrap_last64   = ((sync->unwrap_cycles << 32) | (uint64_t)sync->last_sr_rtp32);
           UNWRAP_LOGD("SSRC 0x%08X unwrap init from SR: last_sr_rtp32=%u, base64=%llu",
                       ssrc, sync->last_sr_rtp32, (unsigned long long)sync->unwrap_last64);
       } else {
           // No SR yet, initialize from first observed RTP
           sync->unwrap_cycles   = 0;
           sync->unwrap_last32   = rtp32;
           sync->unwrap_last64   = ((sync->unwrap_cycles << 32) | (uint64_t)rtp32);
           UNWRAP_LOGD("SSRC 0x%08X unwrap init from first RTP: rtp32=%u, base64=%llu",
                       ssrc, rtp32, (unsigned long long)sync->unwrap_last64);
       }

       // Produce first unwrapped value using current cycles with rtp32
       candidate64 = ((sync->unwrap_cycles << 32) | (uint64_t)rtp32);

       // Mark initialized and advance last seen values, ensuring we don't regress last64
       sync->unwrap_initialized = true;
       sync->unwrap_last32 = rtp32;
       if (candidate64 > sync->unwrap_last64) {
           sync->unwrap_last64 = candidate64;
       }

       *rtp64_out = candidate64;
       xSemaphoreGive(rtcp_mutex);
       return ESP_OK;
   }

   // Already initialized: detect wraps/reorders and build candidate
   uint32_t last32 = sync->unwrap_last32;
   uint64_t cycles = sync->unwrap_cycles;
   uint64_t candidate_cycles = cycles;

   if (rtp32 < last32) {
       // Potential forward wrap
       uint32_t diff = last32 - rtp32;
       if (diff > RTP_WRAP_THRESHOLD) {
           sync->unwrap_cycles++;
           candidate_cycles = sync->unwrap_cycles;
           UNWRAP_LOGI("SSRC 0x%08X RTP wrap forward: cycles %llu->%llu, last32=%u, now=%u",
                       ssrc,
                       (unsigned long long)cycles,
                       (unsigned long long)sync->unwrap_cycles,
                       last32, rtp32);
       }
   } else if (rtp32 > last32) {
       // Potential reordering crossing a wrap boundary (rare)
       uint32_t diff = rtp32 - last32;
       if (diff > RTP_WRAP_THRESHOLD) {
           if (cycles > 0) {
               candidate_cycles = cycles - 1; // map to previous cycle; do not change stored cycles
               UNWRAP_LOGW("SSRC 0x%08X RTP reordered across wrap: cycles=%llu using %llu, last32=%u, now=%u",
                           ssrc,
                           (unsigned long long)cycles,
                           (unsigned long long)candidate_cycles,
                           last32, rtp32);
           } else {
               // Can't go below 0 cycles; will be treated as large backward jump in same cycle
               UNWRAP_LOGW("SSRC 0x%08X large backward jump with cycles=0, last32=%u, now=%u",
                           ssrc, last32, rtp32);
           }
       }
   }

   candidate64 = ((candidate_cycles << 32) | (uint64_t)rtp32);

   // Bound reordering: do not let result regress beyond tolerance relative to last produced 64-bit
   uint64_t last64 = sync->unwrap_last64;
   if (candidate64 + (uint64_t)RTP_REORDER_TOL_TICKS < last64) {
       UNWRAP_LOGW("SSRC 0x%08X RTP reorder beyond tol: cand=%llu last=%llu tol=%u ticks (clamp)",
                   ssrc,
                   (unsigned long long)candidate64,
                   (unsigned long long)last64,
                   (unsigned)RTP_REORDER_TOL_TICKS);
       candidate64 = last64; // clamp; do not regress timeline
   }

   // Update stored last seen values; never decrease unwrap_last64
   sync->unwrap_last32 = rtp32;
   if (candidate64 > sync->unwrap_last64) {
       sync->unwrap_last64 = candidate64;
   }

   *rtp64_out = candidate64;
   xSemaphoreGive(rtcp_mutex);
   return ESP_OK;
}

// Cleanup RTCP receiver
void rtcp_deinit(void) {
    if (rtcp_mutex) {
        xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
        rtcp_state.initialized = false;
        memset(&rtcp_state, 0, sizeof(rtcp_state));
        xSemaphoreGive(rtcp_mutex);
        
        vSemaphoreDelete(rtcp_mutex);
        rtcp_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "RTCP receiver deinitialized");
}

bool rtcp_is_sync_fresh(uint32_t ssrc) {
    if (!rtcp_state.initialized) {
        return false;
    }

    uint64_t rtp_sr_base64 = 0;
    uint64_t ntp_sr_base_us = 0;
    uint64_t mono_sr_base_us = 0;

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            rtp_sr_base64 = rtcp_state.sync_info[i].rtp_sr_base64;
            ntp_sr_base_us = rtcp_state.sync_info[i].ntp_sr_base_us;
            mono_sr_base_us = rtcp_state.sync_info[i].mono_sr_base_us;
            break;
        }
    }
    xSemaphoreGive(rtcp_mutex);

    bool seeded = (rtp_sr_base64 != 0 && ntp_sr_base_us != 0 && mono_sr_base_us != 0);
    if (!seeded) {
        return false;
    }

    uint64_t now_mono_us = esp_timer_get_time();
    uint64_t sr_age_us = (now_mono_us >= mono_sr_base_us) ? (now_mono_us - mono_sr_base_us) : 0ULL;
    uint64_t max_age_us = (uint64_t)CONFIG_RTCP_SR_MAX_AGE_MS * 1000ULL;

    return sr_age_us <= max_age_us;
}

/**
 * @brief Observe buffer error and gently adjust RTCP mapping (offset b and slope a).
 * Applies a low-rate control loop (PLL) to keep effective buffer delay near a nominal target.
 *
 * Preconditions:
 *  - RTCP mapping must be seeded and fresh (rtcp_is_sync_fresh)
 *  - Holds rtcp_mutex only for minimal state updates
 */
void rtcp_pll_observe(uint32_t ssrc, int64_t error_us, uint32_t sample_window_us) {
    if (!rtcp_state.initialized) {
        return;
    }

    // Freshness/seed gate (avoid deadlock by checking outside of the RTCP mutex)
    if (!rtcp_is_sync_fresh(ssrc)) {
        return;
    }

    const double Kb = (double)CONFIG_RTCP_PLL_KB;
    const double Ki = (double)CONFIG_RTCP_PLL_KI;
    const double Ka = (double)CONFIG_RTCP_PLL_KA;
    const uint64_t apply_interval_us = (uint64_t)CONFIG_RTCP_PLL_APPLY_INTERVAL_MS * 1000ULL;
    const double a0 = 1000000.0 / (double)CONFIG_SAMPLE_RATE; // nominal us/tick
    const double ppm_limit = (double)CONFIG_RTCP_PLL_SLOPE_PPM_LIMIT;
    const double offset_step_limit_us = (double)CONFIG_RTCP_PLL_OFFSET_STEP_LIMIT_US;

    uint64_t now = esp_timer_get_time();
    uint32_t window_us = (sample_window_us == 0u) ? 1u : sample_window_us;

    // Skip implausible observations relative to the sample window
    int64_t abs_err = (error_us < 0) ? -error_us : error_us;
    int64_t outlier_thr = (int64_t)((uint64_t)CONFIG_RTCP_PLL_OBS_OUTLIER_FACTOR * (uint64_t)window_us);
    if (abs_err > outlier_thr) {
#ifdef CONFIG_RTCP_LOG_PLL
        static uint32_t pll_skip_log_counter = 0;
        if ((++pll_skip_log_counter % 200) == 0) {
            ESP_LOGW(TAG, "PLL skip outlier obs err=%lldus window=%uus thr=%lldus",
                     (long long)abs_err, (unsigned)window_us, (long long)outlier_thr);
        }
#endif
        return;
    }

    // Clamp error contribution into the integrator to avoid wind-up on outliers
    double target_us = (double)window_us;
    double err_d = (double)error_us;
    double err_clamped = err_d;
    double i_clamp = 4.0 * target_us;
    if (err_clamped > i_clamp) err_clamped = i_clamp;
    if (err_clamped < -i_clamp) err_clamped = -i_clamp;

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);

    // Locate SSRC entry
    rtcp_sync_info_t *sync = NULL;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == ssrc) {
            sync = &rtcp_state.sync_info[i];
            break;
        }
    }
    if (!sync) {
        xSemaphoreGive(rtcp_mutex);
        return;
    }

    // Ensure seeded under lock as well
    bool seeded = (sync->rtp_sr_base64 != 0 && sync->ntp_sr_base_us != 0 && sync->mono_sr_base_us != 0);
    if (!seeded) {
        xSemaphoreGive(rtcp_mutex);
        return;
    }

    // Update integral term on every observation
    sync->pll_i_err += err_clamped;

    // Rate-limit applications
    if (sync->pll_last_apply_mono != 0) {
        uint64_t elapsed = (now >= sync->pll_last_apply_mono) ? (now - sync->pll_last_apply_mono) : 0ULL;
        if (elapsed < apply_interval_us) {
            xSemaphoreGive(rtcp_mutex);
            return;
        }
    }

    // Compute offset (b) correction
    double window_sec = ((double)window_us) / 1000000.0;
    double delta_b = (Kb * err_d) + (Ki * (sync->pll_i_err * window_sec));

    // Bound per-apply offset change to avoid audible steps
    if (delta_b > offset_step_limit_us) delta_b = offset_step_limit_us;
    if (delta_b < -offset_step_limit_us) delta_b = -offset_step_limit_us;

    // Compute slope (a) correction in ppm and apply multiplicatively
    double delta_ppm = Ka * (err_d / (double)window_us) * 1.0e6;

    double cur_a = sync->slope_a_us_per_tick;
    if (!(cur_a > 0.0) || !isfinite(cur_a)) {
        cur_a = a0;
    }
    double new_a = cur_a * (1.0 + (delta_ppm / 1.0e6));

    // Hard clamp overall slope around nominal to +/- ppm_limit
    double min_a = a0 * (1.0 - ppm_limit / 1.0e6);
    double max_a = a0 * (1.0 + ppm_limit / 1.0e6);
    if (new_a < min_a) new_a = min_a;
    if (new_a > max_a) new_a = max_a;

    // Apply updates
    sync->offset_b_mono_us += (int64_t)delta_b;
    sync->pll_offset_b_us  += delta_b;

    sync->slope_a_us_per_tick = new_a;
    sync->pll_slope_a_correction = (new_a / a0) - 1.0; // fractional multiplier minus 1

    sync->pll_last_apply_mono = now;
    sync->pll_obs_count++;

    // Track last delta_b applied in int32 range for diagnostics
    double db = delta_b;
    if (db > 2147483647.0) db = 2147483647.0;
    if (db < -2147483648.0) db = -2147483648.0;
    sync->pll_last_delta_b_us = (int32_t)(db);

#ifdef CONFIG_RTCP_LOG_PLL
    double total_ppm = ((sync->slope_a_us_per_tick / a0) - 1.0) * 1.0e6;
    ESP_LOGI(TAG, "PLL: ssrc=0x%08X err=%lldus db=%0.1fus dppm=%+0.2f a=%0.6fus/tick b=%lldus",
             ssrc,
             (long long)error_us,
             delta_b,
             total_ppm,
             sync->slope_a_us_per_tick,
             (long long)sync->offset_b_mono_us);
#endif

    xSemaphoreGive(rtcp_mutex);
}

// Primary SSRC selection and control APIs (thread-safe)
bool rtcp_get_primary_ssrc(uint32_t *ssrc_out) {
    if (!rtcp_state.initialized) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    uint64_t now = esp_timer_get_time();
    // Validate current primary
    if (rtcp_state.primary_valid) {
        for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
            if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == rtcp_state.primary_ssrc) {
                if (ssrc_out) *ssrc_out = rtcp_state.primary_ssrc;
                ok = true;
                xSemaphoreGive(rtcp_mutex);
                return ok;
            }
        }
        // Primary no longer valid
        rtcp_state.primary_valid = false;
    }

    // Pick most recently active valid SSRC within freshness window
    uint64_t fresh_us = ((uint64_t)CONFIG_RTCP_PRIMARY_REQUIRE_FRESH_MS) * 1000ULL;
    int best_idx = -1;
    uint64_t best_last = 0;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        rtcp_sync_info_t *s = &rtcp_state.sync_info[i];
        if (s->valid) {
            uint64_t last = s->last_activity_mono_us;
            uint64_t age_us = (last == 0ULL || now < last) ? UINT64_MAX : (now - last);
            if (age_us <= fresh_us) {
                if (last >= best_last) {
                    best_last = last;
                    best_idx = i;
                }
            }
        }
    }
    if (best_idx >= 0) {
        rtcp_state.primary_ssrc = rtcp_state.sync_info[best_idx].ssrc;
        rtcp_state.primary_valid = true;
        if (ssrc_out) *ssrc_out = rtcp_state.primary_ssrc;
        ok = true;
    }
    xSemaphoreGive(rtcp_mutex);
    return ok;
}

void rtcp_pin_primary_ssrc(uint32_t ssrc, bool pin) {
    if (!rtcp_state.initialized) {
        return;
    }
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    rtcp_sync_info_t *sync = find_or_allocate_sync_info(ssrc);
    if (sync) {
        sync->preferred_pin = pin;
        if (pin) {
            rtcp_state.primary_ssrc = ssrc;
            rtcp_state.primary_valid = true;
#ifdef CONFIG_RTCP_LOG_SSRC
            ESP_LOGI(TAG, "Primary SSRC pinned to 0x%08X", ssrc);
#endif
        }
    }
    xSemaphoreGive(rtcp_mutex);
}

bool rtcp_consider_primary_switch(uint32_t candidate_ssrc, uint32_t *new_primary_ssrc) {
    if (!rtcp_state.initialized) {
        return false;
    }
    bool switched = false;
    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    uint64_t now = esp_timer_get_time();

    // Respect any pinned valid SSRC
    int pinned_idx = -1;
    uint64_t pinned_last = 0;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        rtcp_sync_info_t *s = &rtcp_state.sync_info[i];
        if (s->valid && s->preferred_pin) {
            if (s->last_activity_mono_us >= pinned_last) {
                pinned_last = s->last_activity_mono_us;
                pinned_idx = i;
            }
        }
    }
    if (pinned_idx >= 0) {
        uint32_t pinned_ssrc = rtcp_state.sync_info[pinned_idx].ssrc;
        if (!rtcp_state.primary_valid || rtcp_state.primary_ssrc != pinned_ssrc) {
            uint32_t old = rtcp_state.primary_ssrc;
            rtcp_state.primary_ssrc = pinned_ssrc;
            rtcp_state.primary_valid = true;
            switched = true;
            if (new_primary_ssrc) *new_primary_ssrc = pinned_ssrc;
#ifdef CONFIG_RTCP_LOG_SSRC
            ESP_LOGI(TAG, "Primary SSRC switch 0x%08X->0x%08X (pinned)", old, pinned_ssrc);
#endif
        }
        xSemaphoreGive(rtcp_mutex);
        return switched;
    }

    // Locate candidate and current
    int cand_idx = -1;
    int cur_idx = -1;
    for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
        if (rtcp_state.sync_info[i].valid) {
            if (rtcp_state.sync_info[i].ssrc == candidate_ssrc) cand_idx = i;
            if (rtcp_state.primary_valid && rtcp_state.sync_info[i].ssrc == rtcp_state.primary_ssrc) cur_idx = i;
        }
    }

    if (cand_idx < 0) {
        xSemaphoreGive(rtcp_mutex);
        return false; // no candidate state
    }

    uint64_t cand_last = rtcp_state.sync_info[cand_idx].last_activity_mono_us;
    uint64_t cand_age_us = (cand_last == 0ULL || now < cand_last) ? UINT64_MAX : (now - cand_last);
    uint64_t fresh_us = ((uint64_t)CONFIG_RTCP_PRIMARY_REQUIRE_FRESH_MS) * 1000ULL;
    bool cand_fresh = cand_age_us <= fresh_us;

    // If no current or current invalid, accept fresh candidate
    if (cur_idx < 0 || !rtcp_state.sync_info[cur_idx].valid) {
        if (cand_fresh) {
            uint32_t old = rtcp_state.primary_ssrc;
            rtcp_state.primary_ssrc = candidate_ssrc;
            rtcp_state.primary_valid = true;
            switched = true;
            if (new_primary_ssrc) *new_primary_ssrc = candidate_ssrc;
#ifdef CONFIG_RTCP_LOG_SSRC
            ESP_LOGI(TAG, "Primary SSRC switch 0x%08X->0x%08X (no current)", old, candidate_ssrc);
#endif
        }
        xSemaphoreGive(rtcp_mutex);
        return switched;
    }

    if (candidate_ssrc == rtcp_state.primary_ssrc) {
        xSemaphoreGive(rtcp_mutex);
        return false; // already primary
    }

    uint64_t cur_last = rtcp_state.sync_info[cur_idx].last_activity_mono_us;
    uint64_t cur_age_us = (cur_last == 0ULL || now < cur_last) ? UINT64_MAX : (now - cur_last);
    uint64_t min_gap_us = ((uint64_t)CONFIG_RTCP_PRIMARY_SWITCH_MIN_GAP_MS) * 1000ULL;

    if (cand_fresh && cur_age_us >= min_gap_us) {
        uint32_t old = rtcp_state.primary_ssrc;
        rtcp_state.primary_ssrc = candidate_ssrc;
        rtcp_state.primary_valid = true;
        switched = true;
        if (new_primary_ssrc) *new_primary_ssrc = candidate_ssrc;
#ifdef CONFIG_RTCP_LOG_SSRC
        uint64_t gap_ms = (cur_age_us / 1000ULL);
        ESP_LOGI(TAG, "Primary SSRC switch 0x%08X->0x%08X (last_gap=%llu ms)", old, candidate_ssrc, (unsigned long long)gap_ms);
#endif
    }

    xSemaphoreGive(rtcp_mutex);
    return switched;
}

// Low‑rate RTCP structured summary; prints once per CONFIG_RTCP_LOG_SUMMARY_INTERVAL_MS
static void rtcp_log_summary_if_due(void) {
#ifndef CONFIG_RTCP_LOG_SYNC_INFO
    return;
#else
    static uint64_t last_sum_us = 0;
    const uint64_t interval_us = ((uint64_t)CONFIG_RTCP_LOG_SUMMARY_INTERVAL_MS) * 1000ULL;
    uint64_t now = esp_timer_get_time();
    if (last_sum_us != 0 && (now - last_sum_us) < interval_us) {
        return;
    }
    last_sum_us = now;

    // Snapshot per-SSRC fields under lock (primary if valid, else most-recent valid)
    uint32_t ssrc = 0;
    double   slope_a_us_per_tick = 0.0;
    int64_t  offset_b_mono_us = 0;
    double   jitter_ts = 0.0;
    int32_t  cumulative_lost = 0;
    uint32_t rr_prev_fraction_lost = 0;
    uint64_t mono_sr_base_us = 0;
    uint64_t ntp_sr_base_us  = 0;
    uint64_t rtp_sr_base64   = 0;
    int32_t  pll_last_delta_b_us = 0;
    uint32_t pll_obs_count = 0;
    double   pll_slope_corr = 0.0;
    bool     have = false;

    xSemaphoreTake(rtcp_mutex, portMAX_DELAY);
    int idx = -1;
    if (rtcp_state.primary_valid) {
        for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
            if (rtcp_state.sync_info[i].valid && rtcp_state.sync_info[i].ssrc == rtcp_state.primary_ssrc) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        uint64_t best_last = 0;
        for (int i = 0; i < RTCP_MAX_SSRC_SOURCES; i++) {
            rtcp_sync_info_t *s = &rtcp_state.sync_info[i];
            if (s->valid) {
                if (s->last_activity_mono_us >= best_last) {
                    best_last = s->last_activity_mono_us;
                    idx = i;
                }
            }
        }
    }
    if (idx >= 0) {
        rtcp_sync_info_t *s = &rtcp_state.sync_info[idx];
        ssrc                    = s->ssrc;
        slope_a_us_per_tick     = s->slope_a_us_per_tick;
        offset_b_mono_us        = s->offset_b_mono_us;
        jitter_ts               = s->jitter_ts;
        cumulative_lost         = s->cumulative_lost;
        rr_prev_fraction_lost   = s->rr_prev_fraction_lost;
        mono_sr_base_us         = s->mono_sr_base_us;
        ntp_sr_base_us          = s->ntp_sr_base_us;
        rtp_sr_base64           = s->rtp_sr_base64;
        pll_last_delta_b_us     = s->pll_last_delta_b_us;
        pll_obs_count           = s->pll_obs_count;
        pll_slope_corr          = s->pll_slope_a_correction;
        have = true;
    }
    xSemaphoreGive(rtcp_mutex);
    if (!have) return;

    // Compute derived values for printing
    const double a0 = 1000000.0 / (double)CONFIG_SAMPLE_RATE;
    if (!(slope_a_us_per_tick > 0.0) || !isfinite(slope_a_us_per_tick)) {
        slope_a_us_per_tick = a0;
    }
    uint64_t sr_age_ms = 0;
    if (mono_sr_base_us != 0 && now >= mono_sr_base_us) {
        sr_age_ms = (now - mono_sr_base_us) / 1000ULL;
    }
    double a_ppm    = ((slope_a_us_per_tick / a0) - 1.0) * 1.0e6;
    double pll_ppm  = (pll_slope_corr) * 1.0e6;
    uint32_t jitter_us = 0;
    if (jitter_ts > 0.0) {
        double j_us = jitter_ts * slope_a_us_per_tick; // jitter in RTP ticks -> us
        if (j_us < 0.0) j_us = 0.0;
        if (j_us > (double)UINT32_MAX) j_us = (double)UINT32_MAX;
        jitter_us = (uint32_t)(j_us + 0.5);
    }
    double b_ms = ((double)offset_b_mono_us) / 1000.0;

    ESP_LOGI(TAG,
             "RTCP sum: ssrc=0x%08X sr_age=%llums a_ppm=%+0.2f b_ms=%+0.2f jitter_us=%u lost=%d frac=%u/256 pll_db_us=%d pll_ppm=%+0.2f obs=%u",
             ssrc,
             (unsigned long long)sr_age_ms,
             a_ppm,
             b_ms,
             jitter_us,
             (int)cumulative_lost,
             (unsigned)rr_prev_fraction_lost,
             (int)pll_last_delta_b_us,
             pll_ppm,
             (unsigned)pll_obs_count);
#endif
}

#endif // CONFIG_RTCP_ENABLED