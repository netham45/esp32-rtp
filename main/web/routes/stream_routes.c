#include "stream_routes.h"

#include "esp_log.h"
#include "lifecycle_manager.h"
#include "sender/network_out.h"
#include "sender/wav_streamer.h"
#include "global.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static esp_err_t wav_stream_handler(httpd_req_t *req)
{
    device_mode_t mode = lifecycle_get_device_mode();
    if (mode != MODE_SENDER_USB && mode != MODE_SENDER_SPDIF) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sender mode inactive");
        return ESP_FAIL;
    }

    if (!rtp_sender_is_running()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RTP sender not running");
        return ESP_FAIL;
    }

    if (!wav_streamer_is_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WAV streamer unavailable");
        return ESP_FAIL;
    }

    esp_err_t err = wav_streamer_open();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stream already in use");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint8_t header[64];
    size_t header_len = 0;
    err = wav_streamer_build_header(header, sizeof(header), &header_len);
    if (err != ESP_OK) {
        wav_streamer_close();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build WAV header");
        return ESP_FAIL;
    }

    err = httpd_resp_send_chunk(req, (const char *)header, header_len);
    if (err != ESP_OK) {
        wav_streamer_close();
        return ESP_FAIL;
    }

    uint8_t pcm_buffer[PCM_CHUNK_SIZE];
    bool had_activity = false;

    while (true) {
        if (!rtp_sender_is_running()) {
            ESP_LOGW(TAG, "Sender stopped, closing stream");
            break;
        }

        size_t received = wav_streamer_read(pcm_buffer, sizeof(pcm_buffer), pdMS_TO_TICKS(200));

        if (received == 0) {
            if (!had_activity) {
                // Avoid busy loop if no audio yet
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }

        had_activity = true;

        err = httpd_resp_send_chunk(req, (const char *)pcm_buffer, received);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Client write failed: 0x%x", err);
            break;
        }
    }

    // Terminate chunked transfer
    httpd_resp_send_chunk(req, NULL, 0);
    wav_streamer_close();
    return ESP_OK;
}

esp_err_t register_stream_routes(httpd_handle_t server)
{
    static const httpd_uri_t wav_stream_uri = {
        .uri = "/stream/audio.wav",
        .method = HTTP_GET,
        .handler = wav_stream_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &wav_stream_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WAV stream route: 0x%x", err);
    }

    return err;
}
