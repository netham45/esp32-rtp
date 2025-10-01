#include "spdif_in.h"
#include "histogram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "string.h"
#include "math.h"
#include "sdkconfig.h"

// Global state
static rmt_channel_handle_t g_rx_channel = NULL;
static TaskHandle_t g_decoder_task = NULL;
static TaskHandle_t g_init_task = NULL;
static RingbufHandle_t g_symbol_buffer = NULL;
static rmt_symbol_word_t *g_rmt_buffer = NULL;
static rmt_receive_config_t g_rx_config;
extern RingbufHandle_t pcm_buffer;

// 256-byte LUT for pulse classification
static uint8_t pulse_lut[256];

#define TIMING_VARIANCE 3

// Preamble patterns - both normal and inverted
#define PREAMBLE_B_0 0xE8
#define PREAMBLE_B_1 0x17
#define PREAMBLE_M_0 0xE2
#define PREAMBLE_M_1 0x1D
#define PREAMBLE_W_0 0xE4
#define PREAMBLE_W_1 0x1B

// Macro to process each duration - uses LUT for pulse classification
#define PROCESS_SYMBOL(dur)                                                          \
    {                                                                                \
        uint32_t ptype = pulse_lut[dur & 0xFF];                                      \
        if (ptype < 3)                                                               \
        {                                                                            \
                                                                                     \
            if (state & 2)                                                           \
            {               /* in_preamble */                                        \
                state ^= 8; /* Toggle level */                                       \
                                                                                     \
                uint32_t bits_to_add = ptype + 1;                                    \
                uint32_t pindex = (preamble_data >> 8) & 0xF;                        \
                uint32_t pattern = preamble_data & 0xFF;                             \
                                                                                     \
                for (uint32_t j = 0; j < bits_to_add && pindex < 8; j++)             \
                {                                                                    \
                    if (state & 8)                                                   \
                        pattern |= (1 << (7 - pindex));                              \
                    pindex++;                                                        \
                }                                                                    \
                                                                                     \
                if (pindex >= 8)                                                     \
                {                                                                    \
                    state &= ~2; /* Clear in_preamble */                             \
                                                                                     \
                    /* Check all 6 valid preambles */                                \
                    if (pattern == PREAMBLE_B_0 || pattern == PREAMBLE_B_1)          \
                    {                                                                \
                        channel = 0;                                                 \
                    }                                                                \
                    else if (pattern == PREAMBLE_M_0 || pattern == PREAMBLE_M_1)     \
                    {                                                                \
                        channel = 0;                                                 \
                    }                                                                \
                    else if (pattern == PREAMBLE_W_0 || pattern == PREAMBLE_W_1)     \
                    {                                                                \
                        channel = 1;                                                 \
                    }                                                                \
                }                                                                    \
                else                                                                 \
                {                                                                    \
                    preamble_data = pattern | (pindex << 8);                         \
                }                                                                    \
            }                                                                        \
            else if (ptype == 2 && !(state & 1))                                     \
            { /* LONG pulse, not expecting_short */                                  \
                /* Start preamble */                                                 \
                state |= 2; /* Set in_preamble */                                    \
                preamble_data = 0;                                                   \
                                                                                     \
                /* Set initial level based on last_data_bit and toggle */            \
                state = (state & ~8) | ((state & 4) << 1); /* Copy bit 2 to bit 3 */ \
                state ^= 8;                                /* Toggle */              \
                                                                                     \
                /* Add 3 bits for LONG */                                            \
                uint32_t pattern = 0;                                                \
                if (state & 8)                                                       \
                    pattern = 0xE0; /* 111 in top 3 bits */                          \
                preamble_data = pattern | (3 << 8);                                  \
                                                                                     \
                /* Reset subframe */                                                 \
                bit_count = 0;                                                       \
                subframe_data = 0;                                                   \
                state &= ~1; /* Clear expecting_short */                             \
            }                                                                        \
            else if (bit_count < 28)                                                 \
            { /* Normal data */                                                      \
                if (state & 1)                                                       \
                { /* expecting_short */                                              \
                    if (ptype == 0)                                                  \
                    { /* SHORT - completes '1' bit */                                \
                        subframe_data |= (1UL << bit_count);                         \
                    }                                                                \
                    bit_count++;                                                     \
                    state &= ~1; /* Clear expecting_short */                         \
                }                                                                    \
                else                                                                 \
                {                                                                    \
                    if (ptype == 1)                                                  \
                    { /* MEDIUM - '0' bit */                                         \
                        bit_count++;                                                 \
                    }                                                                \
                    else if (ptype == 0)                                             \
                    {               /* SHORT - first half of '1' */                  \
                        state |= 1; /* Set expecting_short */                        \
                    }                                                                \
                }                                                                    \
                                                                                     \
                if (bit_count == 28)                                                 \
                {                                                                    \
                    /* Track last bit for next preamble */                           \
                    state = (state & ~4) | ((subframe_data & (1UL << 27)) ? 4 : 0);  \
                                                                                     \
                    /* Extract audio */                                              \
                    int32_t sample = (int32_t)(subframe_data & 0xFFFFFF);            \
                                                                                     \
                    /* Sign extend from 24-bit to 32-bit */                          \
                    if (sample & 0x800000)                                           \
                    {                                                                \
                        sample |= 0xFF000000;                                        \
                    }                                                                \
                                                                                     \
                    /* Convert to 16-bit */                                          \
                    int16_t s16 = (int16_t)(sample >> 8);                            \
                                                                                     \
                    if (channel == 0)                                                \
                    {                                                                \
                        left_sample = s16;                                           \
                    }                                                                \
                    else                                                             \
                    {                                                                \
                        int16_t stereo[2] = {left_sample, s16};                      \
                        xRingbufferSend(pcm_buffer, stereo, sizeof(stereo), 10000);  \
                    }                                                                \
                }                                                                    \
            }                                                                        \
        }                                                                            \
    }

// Initialize thresholds and build LUT
void decoder_init_thresholds(void)
{
    for (int i = 0; i < 256; i++)
    {
        if (i < g_timing.short_pulse_ticks - TIMING_VARIANCE)
        {
            pulse_lut[i] = 3; // UNKNOWN
        }
        else if (i < g_timing.short_pulse_ticks + TIMING_VARIANCE)
        {
            pulse_lut[i] = 0; // SHORT
        }
        else if (i < g_timing.medium_pulse_ticks - TIMING_VARIANCE)
        {
            pulse_lut[i] = 3; // UNKNOWN
        }
        else if (i < g_timing.medium_pulse_ticks + TIMING_VARIANCE)
        {
            pulse_lut[i] = 1; // MEDIUM
        }
        else if (i < g_timing.long_pulse_ticks - TIMING_VARIANCE)
        {
            pulse_lut[i] = 3; // UNKNOWN
        }
        else if (i < g_timing.long_pulse_ticks + TIMING_VARIANCE)
        {
            pulse_lut[i] = 2; // LONG
        }
        else
        {
            pulse_lut[i] = 3; // UNKNOWN
        }
    }
}

// Main decoder task
static void spdif_decoder_task(void *arg)
{
    g_rx_config = (rmt_receive_config_t){
        .signal_range_min_ns = 10,
        .signal_range_max_ns = 10000,
        .flags.en_partial_rx = true,
    };

    ESP_ERROR_CHECK(rmt_enable(g_rx_channel));
    ESP_ERROR_CHECK(rmt_receive(g_rx_channel, g_rmt_buffer,
                                RMT_MEM_BLOCK_SYMBOLS * sizeof(rmt_symbol_word_t),
                                &g_rx_config));

    size_t rx_size = 0;
    rmt_symbol_word_t *symbols = NULL;

    ESP_LOGI("TAG", "Decoder task started, waiting for PCM buffer");
    while (!pcm_buffer) vTaskDelay(100);
    ESP_LOGI("TAG", "PCM buffer found, continuing");
    while (1)
    {
        //ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        symbols = (rmt_symbol_word_t *)xRingbufferReceive(g_symbol_buffer, &rx_size, portMAX_DELAY);
        while (symbols)
        {
            size_t num_symbols = rx_size / sizeof(rmt_symbol_word_t);
            if (!g_timing.timing_discovered)
            {
                collect_pulse_histogram(symbols, num_symbols);
                if (g_timing.total_samples >= MIN_SAMPLES_FOR_ANALYSIS)
                {
                    analyze_pulse_timing();
                }
            }
            else
            {
                if (!pulse_lut[0])
                {
                    decoder_init_thresholds();
                }

                // Static state - keep minimal for cache efficiency
                static uint32_t state = 0; // Bit 0: expecting_short, Bit 1: in_preamble, Bit 2: last_data_bit, Bit 3: last_level
                static uint32_t bit_count = 0;
                static uint32_t subframe_data = 0;
                static uint32_t preamble_data = 0; // Bits 0-7: pattern, Bits 8-11: bit_index
                static uint32_t channel = 0;       // 0=left, 1=right
                static int16_t left_sample;

                // Process all symbols
                for (size_t i = 0; i < num_symbols; i++)
                {
                    PROCESS_SYMBOL(symbols[i].duration0);
                    PROCESS_SYMBOL(symbols[i].duration1);
                }
            }
            vRingbufferReturnItem(g_symbol_buffer, (void *)symbols);
            symbols = (rmt_symbol_word_t *)xRingbufferReceive(g_symbol_buffer, &rx_size, 0);
        }
    }
}

// RMT receive callback (ISR context)
static bool IRAM_ATTR rmt_rx_done_callback(
    rmt_channel_handle_t channel,
    const rmt_rx_done_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;

    if (edata->flags.is_last)
    {
        rmt_receive(g_rx_channel, g_rmt_buffer,
                    RMT_MEM_BLOCK_SYMBOLS * sizeof(rmt_symbol_word_t),
                    &g_rx_config);
    }

    if (edata->num_symbols > 0)
    {
        xRingbufferSendFromISR(g_symbol_buffer,
                              edata->received_symbols,
                              edata->num_symbols * sizeof(rmt_symbol_word_t),
                              &task_woken);
    }

    vTaskNotifyGiveFromISR(g_decoder_task, &task_woken);
    return task_woken == pdTRUE;
}

// Initialize S/PDIF receiver
esp_err_t spdif_receiver_init(int input_pin, void (*init_done_cb)(void))
{
    ESP_LOGI("TAG", "SPDIF Init Called");

    g_symbol_buffer = xRingbufferCreate(SYMBOL_BUFFER_SIZE * sizeof(rmt_symbol_word_t), RINGBUF_TYPE_BYTEBUF);
    if (!g_symbol_buffer)
    {
        return ESP_FAIL;
    }

    g_rmt_buffer = heap_caps_malloc(
        RMT_MEM_BLOCK_SYMBOLS * sizeof(rmt_symbol_word_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_rmt_buffer)
    {
        return ESP_FAIL;
    }
    rmt_rx_channel_config_t rx_channel_cfg = {
        .gpio_num = input_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
        .flags.with_dma = true,
    };

    esp_err_t ret = rmt_new_rx_channel(&rx_channel_cfg, &g_rx_channel);
    if (ret != ESP_OK)
    {
        return ESP_FAIL;
    }

    rmt_rx_event_callbacks_t cbs = {.on_recv_done = rmt_rx_done_callback};
    ret = rmt_rx_register_event_callbacks(g_rx_channel, &cbs, NULL);

    if (ret != ESP_OK)
    {
        return ESP_FAIL;
    }
    
    if (init_done_cb)
    {
        init_done_cb();
    }

    if (xTaskCreatePinnedToCore(spdif_decoder_task, "spdif_decoder",
                                DECODER_TASK_STACK, NULL,
                                DECODER_TASK_PRIORITY, &g_decoder_task, 1) != pdPASS)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t spdif_receiver_start(void)
{
    return ESP_OK;
}

esp_err_t spdif_receiver_stop(void)
{
    if (g_rx_channel)
    {
        return rmt_disable(g_rx_channel);
    }
    return ESP_OK;
}

void spdif_receiver_deinit(void)
{
    if (g_rx_channel)
    {
        rmt_disable(g_rx_channel);
        rmt_del_channel(g_rx_channel);
        g_rx_channel = NULL;
    }
    if (g_rmt_buffer)
    {
        heap_caps_free(g_rmt_buffer);
        g_rmt_buffer = NULL;
    }
    if (g_symbol_buffer)
    {
        vRingbufferDelete(g_symbol_buffer);
        g_symbol_buffer = NULL;
    }
}

uint32_t spdif_receiver_get_sample_rate(void)
{
    if (!g_timing.timing_discovered)
    {
        return 0;
    }

    uint32_t T = g_timing.base_unit_ticks;

    switch (T)
    {
        case 13:
            return 48000;
        case 14:
            return 44100;
        default:
            return 0;
    }
}