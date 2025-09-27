#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include "spdif_in.h"

extern struct g_timing_t {
    uint32_t histogram[256];
    uint32_t total_samples;
    uint32_t base_unit_ticks;
    uint32_t short_pulse_ticks;
    uint32_t medium_pulse_ticks;
    uint32_t long_pulse_ticks;
    uint32_t short_medium_threshold;
    uint32_t medium_long_threshold;
    bool timing_discovered;
    uint32_t last_analysis_time;
    timing_validation_t last_validation;
} g_timing;

void analyze_pulse_timing(void);
void collect_pulse_histogram(rmt_symbol_word_t *symbols, size_t num_symbols);

#endif // HISTOGRAM_H