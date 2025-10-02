#include "usb_in.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "usb_device_uac.h"

static const char *TAG = "usb_in";

// State management structure
static struct {
    bool initialized;
    bool running;
    bool connected;
    bool receiving_audio;
    TaskHandle_t audio_task;
    void (*init_done_cb)(void);
    
    
    // Statistics
    uint32_t packets_sent;
    uint32_t packets_dropped;
    uint32_t buffer_underruns;
} g_usb_state = {0};

// UAC Callback Functions
// Called when host sends audio to device (speaker output)
// This RECEIVES audio FROM host and writes TO our PCM buffer
static esp_err_t usb_audio_output_callback(uint8_t *buf, size_t len, void *ctx)
{
    // Check if we're running
    if (!g_usb_state.running || !usb_in_pcm_buffer) {
        // Discard audio if not running
        return ESP_OK;
    }
    
    
    // Write the received audio data to PCM ring buffer
    BaseType_t result = xRingbufferSend(usb_in_pcm_buffer, buf, len, 0);
    
    if (result == pdTRUE) {
        g_usb_state.packets_sent++;  // Actually packets received from host
        g_usb_state.receiving_audio = true;
        
        // Debug log periodically
        static int log_count = 0;
        if (++log_count % 1000 == 0) {
            ESP_LOGD(TAG, "USB audio received: %zu bytes, total packets: %lu",
                     len, g_usb_state.packets_sent);
        }
    } else {
        // Buffer full, drop the data
        g_usb_state.packets_dropped++;
        ESP_LOGW(TAG, "PCM buffer full, dropped %zu bytes (total dropped: %lu)",
                 len, g_usb_state.packets_dropped);
    }
    
    return ESP_OK;
}

// Mute callback
static void usb_mute_callback(uint32_t mute, void *ctx)
{
    ESP_LOGI(TAG, "USB audio mute state changed: %s", mute ? "MUTED" : "UNMUTED");
}

// Volume callback
static void usb_volume_callback(uint32_t volume, void *ctx)
{
    ESP_LOGI(TAG, "USB audio volume changed: %d", volume);
}

// Audio monitoring task - now just for status monitoring
static void usb_audio_task(void *arg)
{
    ESP_LOGI(TAG, "USB audio monitoring task started on core %d", xPortGetCoreID());
    
    uint32_t last_packets_sent = 0;
    
    while (g_usb_state.running) {
        // Check activity every second
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Log statistics if there's activity
        if (g_usb_state.packets_sent != last_packets_sent) {
            ESP_LOGD(TAG, "USB audio active: %lu packets received from host, %lu dropped",
                     g_usb_state.packets_sent, g_usb_state.packets_dropped);
            last_packets_sent = g_usb_state.packets_sent;
            g_usb_state.receiving_audio = true;
        } else if (g_usb_state.receiving_audio) {
            g_usb_state.receiving_audio = false;
            ESP_LOGD(TAG, "USB audio inactive");
        }
    }
    
    ESP_LOGI(TAG, "USB audio monitoring task exiting");
    vTaskDelete(NULL);
}


// Initialize USB audio input (acts as USB speaker/sound card)
esp_err_t usb_in_init(void (*init_done_cb)(void))
{
    esp_err_t ret = ESP_OK;
    
    if (g_usb_state.initialized) {
        ESP_LOGW(TAG, "USB input already initialized");
        return ESP_OK;
    }

    usb_in_pcm_buffer = xRingbufferCreate(PCM_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!usb_in_pcm_buffer)
    {
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Initializing USB audio input (USB speaker device)");
    
    // Store init callback
    g_usb_state.init_done_cb = init_done_cb;
    
    // Initialize UAC device - it will handle USB PHY and TinyUSB internally
    uac_device_config_t uac_config = {
        .skip_tinyusb_init = false,  // Let UAC component handle USB PHY and TinyUSB init
        .input_cb = NULL,  // We're not sending audio TO host (not a microphone)
        .output_cb = usb_audio_output_callback,  // For receiving audio FROM host (speaker)
        .set_mute_cb = usb_mute_callback,
        .set_volume_cb = usb_volume_callback,
        .cb_ctx = NULL
    };
    
    ret = uac_device_init(&uac_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UAC device: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "UAC device initialized");
    
    // Clear statistics
    g_usb_state.packets_sent = 0;
    g_usb_state.packets_dropped = 0;
    g_usb_state.buffer_underruns = 0;
    
    // Set initialized flag
    g_usb_state.initialized = true;
    
    // Call init done callback if provided
    if (g_usb_state.init_done_cb) {
        g_usb_state.init_done_cb();
    }
    
    ESP_LOGI(TAG, "USB input initialization complete");
    return ESP_OK;
    
cleanup:
    
    ESP_LOGE(TAG, "USB input initialization failed: %s", esp_err_to_name(ret));
    return ret;
}

// Deinitialize USB audio input
void usb_in_deinit(void)
{
    if (!g_usb_state.initialized) {
        ESP_LOGW(TAG, "USB input not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing USB input");
    
    // Stop if running
    if (g_usb_state.running) {
        usb_in_stop();
    }
    
    // UAC component handles USB PHY cleanup internally
    
    
    // Delete PCM buffer
    if (usb_in_pcm_buffer) {
        vRingbufferDelete(usb_in_pcm_buffer);
        usb_in_pcm_buffer = NULL;
        ESP_LOGI(TAG, "PCM buffer deleted");
    }
    
    // Clear initialized flag and other state
    memset(&g_usb_state, 0, sizeof(g_usb_state));
    
    ESP_LOGI(TAG, "USB input deinitialized");
}

// Start USB audio input
esp_err_t usb_in_start(void)
{
    if (!g_usb_state.initialized) {
        ESP_LOGE(TAG, "USB input not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_usb_state.running) {
        ESP_LOGW(TAG, "USB input already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting USB input (listening for audio from host)");
    
    // Reset statistics
    g_usb_state.packets_sent = 0;
    g_usb_state.packets_dropped = 0;
    g_usb_state.buffer_underruns = 0;
    
    // Set running flag
    g_usb_state.running = true;
    g_usb_state.receiving_audio = false;
    
    // Create audio processing task on core 1
    BaseType_t ret = xTaskCreatePinnedToCore(
        usb_audio_task,
        "usb_audio",
        USB_TASK_STACK_SIZE,
        NULL,
        USB_TASK_PRIORITY,
        &g_usb_state.audio_task,
        1
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB audio task");
        g_usb_state.running = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "USB input started");
    return ESP_OK;
}

// Stop USB audio input
esp_err_t usb_in_stop(void)
{
    if (!g_usb_state.initialized) {
        ESP_LOGE(TAG, "USB input not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!g_usb_state.running) {
        ESP_LOGW(TAG, "USB input not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping USB input");
    
    // Set running flag to false to signal task to exit
    g_usb_state.running = false;
    
    // Wait briefly for task to exit (max 200ms)
    for (int i = 0; i < 20 && g_usb_state.audio_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (eTaskGetState(g_usb_state.audio_task) == eDeleted) {
            break;
        }
    }
    
    // Force delete task if still running
    if (g_usb_state.audio_task != NULL) {
        ESP_LOGW(TAG, "Force deleting USB audio task");
        vTaskDelete(g_usb_state.audio_task);
    }
    g_usb_state.audio_task = NULL;
    
    // Log final statistics
    ESP_LOGI(TAG, "USB input stopped - Statistics:");
    ESP_LOGI(TAG, "  Packets received from host: %lu", g_usb_state.packets_sent);
    ESP_LOGI(TAG, "  Packets dropped: %lu", g_usb_state.packets_dropped);
    ESP_LOGI(TAG, "  Buffer underruns: %lu", g_usb_state.buffer_underruns);
    
    return ESP_OK;
}

// Get sample rate
uint32_t usb_in_get_sample_rate(void)
{
    return USB_SAMPLE_RATE;
}

// Check if USB is connected
bool usb_in_is_connected(void)
{
    // Since we removed our tud_mount_cb, we need a different way to check connection
    // The UAC component handles this internally, so we just track our running state
    return g_usb_state.initialized && g_usb_state.running;
}