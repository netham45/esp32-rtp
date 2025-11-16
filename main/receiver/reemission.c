#include "reemission.h"

#include "audio_out.h"
#include "global.h"
#include "lifecycle_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

#define REEMISSION_TASK_STACK_WORDS 4096
#define REEMISSION_TASK_PRIORITY    5


static esp_timer_handle_t s_reemit_timer = NULL;
static TaskHandle_t s_reemit_task = NULL;
static bool s_initialized = false;
static bool s_running = false;
static uint32_t s_period_us = 0;

static void reemission_task(void *arg);

static esp_err_t reemission_compute_period(uint32_t *out_period_us) {
    if (!out_period_us) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t sample_rate = lifecycle_get_sample_rate();
    uint32_t bit_depth = lifecycle_get_bit_depth();
    const uint32_t channels = 2; // Stereo output is currently fixed

    if (sample_rate == 0 || bit_depth == 0) {
        ESP_LOGW(TAG, "Invalid audio params (sr=%" PRIu32 ", bd=%" PRIu32 ")", sample_rate, bit_depth);
        return ESP_ERR_INVALID_STATE;
    }

    if (bit_depth % 8u != 0u) {
        ESP_LOGW(TAG, "Unsupported bit depth %" PRIu32 " (not byte aligned)", bit_depth);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t bytes_per_sample = bit_depth / 8u;
    uint64_t bytes_per_second = (uint64_t)sample_rate * (uint64_t)channels * (uint64_t)bytes_per_sample;
    if (bytes_per_second == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t numerator = (uint64_t)PCM_CHUNK_SIZE * 1000000ULL;
    uint32_t period_us = (uint32_t)(numerator / bytes_per_second);
    if (period_us == 0) {
        period_us = 1; // Guard against extremely large sample rates
    }

    *out_period_us = period_us;
    return ESP_OK;
}

static void reemission_timer_callback(void *arg) {
    if (s_reemit_task) {
        xTaskNotifyGive(s_reemit_task);
    }
}

esp_err_t reemission_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_timer_create_args_t timer_args = {
        .callback = reemission_timer_callback,
        .arg = NULL,
        .name = "reemission"
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_reemit_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        reemission_task,
        "reemission",
        REEMISSION_TASK_STACK_WORDS,
        NULL,
        REEMISSION_TASK_PRIORITY,
        &s_reemit_task,
        tskNO_AFFINITY);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reemission task");
        esp_timer_delete(s_reemit_timer);
        s_reemit_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t reemission_apply_period(uint32_t new_period_us) {
    if (!s_reemit_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    s_period_us = new_period_us;
    if (s_running) {
        esp_timer_stop(s_reemit_timer);
    }
    esp_err_t err = esp_timer_start_periodic(s_reemit_timer, s_period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start reemission timer: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "Reemission period set to %u us", s_period_us);
    return ESP_OK;
}

esp_err_t reemission_start(void) {
    if (!s_initialized) {
        esp_err_t err = reemission_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    uint32_t period_us = 0;
    esp_err_t err = reemission_compute_period(&period_us);
    if (err != ESP_OK) {
        return err;
    }

    return reemission_apply_period(period_us);
}

void reemission_stop(void) {
    if (!s_initialized || !s_reemit_timer) {
        return;
    }

    if (s_running) {
        esp_timer_stop(s_reemit_timer);
    }
    s_running = false;
}

esp_err_t reemission_update_audio_params(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t period_us = 0;
    esp_err_t err = reemission_compute_period(&period_us);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_running) {
        s_period_us = period_us;
        return ESP_OK;
    }

    return reemission_apply_period(period_us);
}

static void reemission_task(void *arg) {
    while (1) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        if (!s_running) {
            continue;
        }

        uint64_t now_us = esp_timer_get_time();
        audio_out_reemission_tick(now_us);
    }
}
