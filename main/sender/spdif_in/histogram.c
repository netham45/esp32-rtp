#include "histogram.h"
#include "spdif_in.h"
#include "esp_log.h"
#include "string.h"
#include "math.h"

static const char *TAG = "spdif_histogram";

// Pulse timing analysis constants
#define HISTOGRAM_BINS 256             // Number of histogram bins for pulse width
#define MAX_PULSE_WIDTH_NS 2000        // Maximum expected pulse width in nanoseconds
#define MIN_SAMPLES_FOR_ANALYSIS 10000 // Minimum samples before attempting analysis
#define PULSE_RATIO_TOLERANCE 0.15     // 15% tolerance for pulse ratio matching

// S/PDIF spec distribution expectations
#define EXPECTED_SHORT_PULSE_PCT 60.0f  // ~60% short pulses
#define EXPECTED_MEDIUM_PULSE_PCT 35.0f // ~35% medium pulses
#define EXPECTED_LONG_PULSE_PCT 5.0f    // ~5% long pulses (preambles)
#define DISTRIBUTION_TOLERANCE 100.0f   // ±100% tolerance for distribution

// Pulse timing analysis data
struct g_timing_t g_timing = {0};

// Helper function to smooth histogram data (3-point moving average)
static void smooth_histogram(uint32_t *input, uint32_t *output, int size)
{
    output[0] = input[0];
    output[size - 1] = input[size - 1];
    for (int i = 1; i < size - 1; i++)
    {
        output[i] = (input[i - 1] + input[i] + input[i + 1]) / 3;
    }
}

// Helper function to find the center of mass for a peak
static float find_peak_center(uint32_t *histogram, int peak_bin, int window)
{
    float weighted_sum = 0;
    float weight_total = 0;
    int start = (peak_bin - window >= 0) ? peak_bin - window : 0;
    int end = (peak_bin + window < HISTOGRAM_BINS) ? peak_bin + window : HISTOGRAM_BINS - 1;
    for (int i = start; i <= end; i++)
    {
        weighted_sum += i * histogram[i];
        weight_total += histogram[i];
    }
    return (weight_total > 0) ? weighted_sum / weight_total : peak_bin;
}

// Calculate adaptive thresholds between pulse groups
static void calculate_adaptive_thresholds(void)
{
    if (!g_timing.timing_discovered) return;
    g_timing.short_medium_threshold = (g_timing.short_pulse_ticks + g_timing.medium_pulse_ticks) / 2;
    g_timing.medium_long_threshold = (g_timing.medium_pulse_ticks + g_timing.long_pulse_ticks) / 2;
}

// Validate pulse distribution against S/PDIF specification
static timing_validation_t validate_pulse_distribution(
    peak_t *peaks, int num_peaks, float ratio1, float ratio2, float best_error)
{
    timing_validation_t result = {0};
    result.groups_identified = (num_peaks >= 3);
    if (!result.groups_identified) return result;

    result.ratio_error = best_error;
    result.ratios_valid = (fabs(ratio1 - 2.0) < PULSE_RATIO_TOLERANCE &&
                           fabs(ratio2 - 3.0) < PULSE_RATIO_TOLERANCE);

    uint32_t total = peaks[0].count + peaks[1].count + peaks[2].count;
    if (total > 0)
    {
        result.short_pulse_pct = 100.0f * peaks[0].count / total;
        result.medium_pulse_pct = 100.0f * peaks[1].count / total;
        result.long_pulse_pct = 100.0f * peaks[2].count / total;

        float short_error = fabs(result.short_pulse_pct - EXPECTED_SHORT_PULSE_PCT);
        float medium_error = fabs(result.medium_pulse_pct - EXPECTED_MEDIUM_PULSE_PCT);
        float long_error = fabs(result.long_pulse_pct - EXPECTED_LONG_PULSE_PCT);
        result.distribution_error = short_error + medium_error + long_error;
        result.distribution_valid = (short_error <= DISTRIBUTION_TOLERANCE &&
                                     medium_error <= DISTRIBUTION_TOLERANCE &&
                                     long_error <= DISTRIBUTION_TOLERANCE);
    }
    return result;
}

// Helper function to analyze pulse timing histogram
void analyze_pulse_timing(void)
{
    uint32_t smoothed[HISTOGRAM_BINS];
    smooth_histogram(g_timing.histogram, smoothed, HISTOGRAM_BINS);

    peak_t peaks[10] = {0};
    int num_peaks = 0;
    uint32_t max_count = 0;
    for (int i = 0; i < HISTOGRAM_BINS; i++) {
        if (smoothed[i] > max_count) max_count = smoothed[i];
    }
    uint32_t min_peak_height = (max_count / 50 > g_timing.total_samples / 200) ? max_count / 50 : g_timing.total_samples / 200;

    for (int i = 2; i < HISTOGRAM_BINS - 2 && num_peaks < 10; i++) {
        if (smoothed[i] > min_peak_height &&
            smoothed[i] >= smoothed[i - 1] && smoothed[i] >= smoothed[i - 2] &&
            smoothed[i] >= smoothed[i + 1] && smoothed[i] >= smoothed[i + 2]) {
            bool is_distinct = true;
            for (int j = 0; j < num_peaks; j++) {
                if (abs((int)i - (int)peaks[j].bin) < 8) {
                    if (smoothed[i] > peaks[j].count) {
                        peaks[j].bin = i;
                        peaks[j].count = smoothed[i];
                        peaks[j].center = find_peak_center(smoothed, i, 3);
                    }
                    is_distinct = false;
                    break;
                }
            }
            if (is_distinct) {
                peaks[num_peaks].bin = i;
                peaks[num_peaks].count = smoothed[i];
                peaks[num_peaks].center = find_peak_center(smoothed, i, 3);
                num_peaks++;
            }
        }
    }

    if (num_peaks < 3) return;

    for (int i = 0; i < num_peaks - 1; i++) {
        for (int j = i + 1; j < num_peaks; j++) {
            if (peaks[i].center > peaks[j].center) {
                peak_t temp = peaks[i];
                peaks[i] = peaks[j];
                peaks[j] = temp;
            }
        }
    }

    int best_set[3] = {-1, -1, -1};
    float best_error = 1000.0;
    for (int i = 0; i < num_peaks - 2; i++) {
        for (int j = i + 1; j < num_peaks - 1; j++) {
            for (int k = j + 1; k < num_peaks; k++) {
                float ratio1 = peaks[j].center / peaks[i].center;
                float ratio2 = peaks[k].center / peaks[i].center;
                float error = fabs(ratio1 - 2.0) + fabs(ratio2 - 3.0);
                if (error < best_error) {
                    best_error = error;
                    best_set[0] = i; best_set[1] = j; best_set[2] = k;
                }
            }
        }
    }

    if (best_set[0] >= 0 && best_error < PULSE_RATIO_TOLERANCE * 2) {
        peak_t selected_peaks[3];
        for (int i = 0; i < 3; i++) selected_peaks[i] = peaks[best_set[i]];
        
        float ratio1 = selected_peaks[1].center / selected_peaks[0].center;
        float ratio2 = selected_peaks[2].center / selected_peaks[0].center;

        timing_validation_t validation = validate_pulse_distribution(selected_peaks, 3, ratio1, ratio2, best_error);
        g_timing.last_validation = validation;

        if (validation.groups_identified && validation.ratios_valid && validation.distribution_valid) {
            g_timing.base_unit_ticks = (uint32_t)(selected_peaks[0].center * 2); // Short pulse is 0.5T
            g_timing.short_pulse_ticks = (uint32_t)selected_peaks[0].center;     // 0.5T
            g_timing.medium_pulse_ticks = (uint32_t)selected_peaks[1].center;    // 1.0T
            g_timing.long_pulse_ticks = (uint32_t)selected_peaks[2].center;      // 1.5T
            g_timing.timing_discovered = true;
            calculate_adaptive_thresholds();
        }
    }
}

// Helper function to collect pulse width histogram
void collect_pulse_histogram(rmt_symbol_word_t *symbols, size_t num_symbols)
{
    for (size_t i = 0; i < num_symbols; i++)
    {
        uint32_t dur0 = symbols[i].duration0;
        uint32_t dur1 = symbols[i].duration1;
        if (dur0 > 0 && dur0 < HISTOGRAM_BINS) {
            g_timing.histogram[dur0]++;
            g_timing.total_samples++;
        }
        if (dur1 > 0 && dur1 < HISTOGRAM_BINS) {
            g_timing.histogram[dur1]++;
            g_timing.total_samples++;
        }
    }
}