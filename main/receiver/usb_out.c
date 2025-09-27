#include "receiver/usb_out.h"
#include "esp_err.h"
#include "esp_log.h"
#include "config.h"

#ifdef IS_USB
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "global.h"
#include "receiver/audio_out.h"
#include "config.h"
#include "lifecycle_manager.h"

#define TAG "usb_out"

// Global USB speaker device handle (exposed for web_server.c power management)
uac_host_device_handle_t s_spk_dev_handle = NULL;

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define USB_HOST_TASK_STACK_SIZE 4096
#define UAC_TASK_STACK_SIZE     4096
#define EVENT_QUEUE_SIZE        10

// Timeout and retry configuration
#define USB_ENUMERATION_TIMEOUT_MS   5000  // Maximum time to wait for device enumeration
#define USB_TRANSFER_RETRY_COUNT     3     // Maximum number of transfer retries
#define USB_TRANSFER_RETRY_DELAY_MS  100   // Initial delay between retries (exponential backoff)
#define USB_RECONNECT_DELAY_MS       2000  // Delay before attempting reconnection
#define USB_RECONNECT_MAX_ATTEMPTS   5     // Maximum reconnection attempts
#define USB_INIT_MAX_RETRIES        3     // Maximum USB host initialization retries

typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

// Store device parameters for fast reconnection after sleep
typedef struct {
    uint8_t addr;
    uint8_t iface_num;
    uac_host_stream_config_t stream_config;
    bool valid;
} saved_usb_device_t;

// USB Host and UAC state management
typedef struct {
    uac_host_device_handle_t spk_dev_handle;
    QueueHandle_t event_queue;
    TaskHandle_t usb_host_task_handle;
    TaskHandle_t uac_task_handle;
    bool usb_host_running;
    bool initialized;
    saved_usb_device_t saved_device;  // Saved device parameters for sleep/wake
    // Error handling and recovery
    uint32_t transfer_error_count;
    uint32_t transfer_retry_count;
    uint32_t reconnect_attempts;
    TickType_t last_error_time;
    TickType_t last_reconnect_time;
    bool device_enumeration_complete;
    TickType_t enumeration_start_time;
} usb_out_state_t;

static usb_out_state_t s_usb_state = {
    .spk_dev_handle = NULL,
    .event_queue = NULL,
    .usb_host_task_handle = NULL,
    .uac_task_handle = NULL,
    .usb_host_running = false,
    .initialized = false,
    .saved_device = { .valid = false },
    .transfer_error_count = 0,
    .transfer_retry_count = 0,
    .reconnect_attempts = 0,
    .last_error_time = 0,
    .last_reconnect_time = 0,
    .device_enumeration_complete = false,
    .enumeration_start_time = 0
};

typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_driver_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;

// Forward declarations for new functions
static esp_err_t usb_out_save_device_params(uint8_t addr, uint8_t iface_num, const uac_host_stream_config_t *stream_config);
static esp_err_t usb_out_restore_device(void);
static esp_err_t usb_out_handle_transfer_error(void);
static esp_err_t usb_out_attempt_reconnection(void);
static bool usb_out_validate_handle(uac_host_device_handle_t handle);
static void usb_out_reset_error_counters(void);

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    // Validate device handle
    if (!usb_out_validate_handle(uac_device_handle)) {
        ESP_LOGE(TAG, "Invalid device handle in callback");
        return;
    }
    
    // Handle disconnect event immediately
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "UAC Device disconnected, attempting recovery");
        
        // Save device parameters before disconnection if we're entering sleep mode
        // Note: These will be saved properly when device is first configured
        if (s_usb_state.saved_device.valid) {
            ESP_LOGI(TAG, "Device parameters already saved for reconnection");
        }
        
        // Stop audio player first
        s_usb_state.spk_dev_handle = NULL;
        s_spk_dev_handle = NULL;  // Update global handle for web_server.c
        stop_playback();
        
        // Close the device handle
        esp_err_t err = uac_host_device_close(uac_device_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to close device handle: %s", esp_err_to_name(err));
        }
        
        // Schedule reconnection attempt
        s_usb_state.last_reconnect_time = xTaskGetTickCount();
        if (s_usb_state.reconnect_attempts < USB_RECONNECT_MAX_ATTEMPTS) {
            ESP_LOGI(TAG, "Scheduling reconnection attempt %lu/%d in %d ms",
                     (unsigned long)(s_usb_state.reconnect_attempts + 1),
                     USB_RECONNECT_MAX_ATTEMPTS,
                     USB_RECONNECT_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(USB_RECONNECT_DELAY_MS));
            
            if (!is_playing()) {
                s_usb_state.reconnect_attempts++;
                usb_out_attempt_reconnection();
            }
        } else {
            ESP_LOGW(TAG, "Maximum reconnection attempts reached (%d), giving up",
                     USB_RECONNECT_MAX_ATTEMPTS);
            // Lifecycle manager can trigger sleep mode if configured
        }
        return;
    }
    
    // Send device event to the event queue for further processing
    s_event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    // Should not block here
    if (s_usb_state.event_queue) {
        BaseType_t ret = xQueueSend(s_usb_state.event_queue, &evt_queue, 0);
        if (ret != pdTRUE) {
            ESP_LOGW(TAG, "Failed to send device event to queue (queue full)");
        }
    }
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    s_event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    if (s_usb_state.event_queue) {
        xQueueSend(s_usb_state.event_queue, &evt_queue, 0);
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host library installed");
    xTaskNotifyGive(arg);

    while (s_usb_state.usb_host_running) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        
        // Handle USB host library events
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "No clients connected");
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "All devices freed");
            break;
        }
    }

    ESP_LOGI(TAG, "USB Host library shutting down");
    vTaskDelay(10); // Short delay to allow cleanup
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void uac_lib_task(void *arg)
{
    // Wait for USB host to be ready
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = UAC_TASK_STACK_SIZE,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
    
    s_event_queue_t evt_queue = {0};
    while (s_usb_state.usb_host_running) {
        if (xQueueReceive(s_usb_state.event_queue, &evt_queue, portMAX_DELAY)) {
            if (UAC_DRIVER_EVENT == evt_queue.event_group) {
                uac_host_driver_event_t event = evt_queue.driver_evt.event;
                uint8_t addr = evt_queue.driver_evt.addr;
                uint8_t iface_num = evt_queue.driver_evt.iface_num;
                
                switch (event) {
                    case UAC_HOST_DRIVER_EVENT_TX_CONNECTED: {
                        // Audio output device connected
                        if (s_usb_state.spk_dev_handle == NULL) {
                            uac_host_dev_info_t dev_info;
                            uac_host_device_handle_t uac_device_handle = NULL;
                            
                            // Configure device with proper buffer size
                            const uac_host_device_config_t dev_config = {
                                .addr = addr,
                                .iface_num = iface_num,
                                .buffer_size = PCM_CHUNK_SIZE * 4,
                                .buffer_threshold = 0,
                                .callback = uac_device_callback,
                                .callback_arg = NULL,
                            };
                            
                            // Open the UAC device
                            esp_err_t err = uac_host_device_open(&dev_config, &uac_device_handle);
                            if (err == ESP_OK) {
                                ESP_LOGI(TAG, "UAC Speaker device opened successfully");
                                
                                // Get and log device information
                                err = uac_host_get_device_info(uac_device_handle, &dev_info);
                                if (err == ESP_OK) {
                                    ESP_LOGI(TAG, "UAC Device connected: SPK");
                                    ESP_LOGI(TAG, "  VID: 0x%04X, PID: 0x%04X", dev_info.VID, dev_info.PID);
                                    ESP_LOGI(TAG, "  iProduct: %s", dev_info.iProduct ? (char*)dev_info.iProduct : "N/A");
                                    ESP_LOGI(TAG, "  iManufacturer: %s", dev_info.iManufacturer ? (char*)dev_info.iManufacturer : "N/A");
                                    
                                    // Print detailed device parameters
                                    uac_host_printf_device_param(uac_device_handle);
                                } else {
                                    ESP_LOGW(TAG, "Failed to get device info: %s", esp_err_to_name(err));
                                }
                                
                                // Configure the audio stream using lifecycle manager
                                uac_host_stream_config_t stm_config = {
                                    .channels = 2,
                                    .bit_resolution = lifecycle_get_bit_depth(),
                                    .sample_freq = lifecycle_get_sample_rate(),
                                };
                                
                                ESP_LOGI(TAG, "Starting device with SR: %lu Hz, BD: %d bits",
                                         (unsigned long)lifecycle_get_sample_rate(), lifecycle_get_bit_depth());
                                
                                // Start the device with the stream configuration
                                err = uac_host_device_start(uac_device_handle, &stm_config);
                                if (err == ESP_OK) {
                                    // Save device parameters for potential reconnection after sleep
                                    usb_out_save_device_params(addr, iface_num, &stm_config);
                                    
                                    // Set volume from lifecycle manager
                                    float volume_percent = lifecycle_get_volume() * 100.0f;
                                    err = uac_host_device_set_volume(uac_device_handle, volume_percent);
                                    if (err == ESP_OK) {
                                        ESP_LOGI(TAG, "Volume set to %.0f%%", volume_percent / 100.0f);
                                    } else {
                                        ESP_LOGW(TAG, "Failed to set volume: %s", esp_err_to_name(err));
                                    }
                                    
                                    // Save the device handle and start playback
                                    s_usb_state.spk_dev_handle = uac_device_handle;
                                    s_spk_dev_handle = uac_device_handle;  // Update global handle for web_server.c
                                    s_usb_state.device_enumeration_complete = true;
                                    s_usb_state.reconnect_attempts = 0;  // Reset reconnection counter on success
                                    usb_out_reset_error_counters();
                                    ESP_LOGI(TAG, "USB audio device ready for playback");

                                    // Start playback (this will set the audio_device_handle and playing flag)
                                    start_playback((audio_device_handle_t)uac_device_handle);
                                } else {
                                    ESP_LOGE(TAG, "Failed to start UAC device: %s", esp_err_to_name(err));
                                    // Close the device if we couldn't start it
                                    uac_host_device_close(uac_device_handle);
                                }
                            } else {
                                ESP_LOGE(TAG, "Failed to open UAC device: %s", esp_err_to_name(err));
                            }
                        } else {
                            ESP_LOGW(TAG, "Speaker device already connected");
                        }
                        break;
                    }
                    
                    case UAC_HOST_DRIVER_EVENT_RX_CONNECTED:
                        // Microphone connected (not supported in this implementation)
                        ESP_LOGI(TAG, "UAC Microphone device connected (not supported)");
                        break;
                        
                    default:
                        ESP_LOGW(TAG, "Unhandled UAC driver event: %d", event);
                        break;
                }
            } else if (UAC_DEVICE_EVENT == evt_queue.event_group) {
                // Handle device-specific events
                uac_host_device_event_t event = evt_queue.device_evt.event;
                uac_host_device_handle_t device_handle = evt_queue.device_evt.handle;
                
                switch (event) {
                    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                        ESP_LOGI(TAG, "UAC Device disconnected (from device event)");
                        // Clean up device handle properly
                        if (device_handle == s_usb_state.spk_dev_handle) {
                            s_usb_state.spk_dev_handle = NULL;
                            s_spk_dev_handle = NULL;  // Update global handle for web_server.c
                            // Device parameters remain saved for potential reconnection
                            ESP_LOGI(TAG, "Device handle cleaned up, parameters saved for reconnection");
                        }
                        break;
                        
                    case UAC_HOST_DEVICE_EVENT_RX_DONE:
                        // Microphone data received (not used in this implementation)
                        break;
                        
                    case UAC_HOST_DEVICE_EVENT_TX_DONE:
                        // Audio data sent successfully
                        ESP_LOGD(TAG, "Audio TX completed");
                        break;
                        
                    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                        ESP_LOGW(TAG, "UAC Transfer error occurred (count: %lu)",
                                (unsigned long)++s_usb_state.transfer_error_count);
                        s_usb_state.last_error_time = xTaskGetTickCount();
                        
                        // Attempt recovery with exponential backoff
                        esp_err_t recovery_result = usb_out_handle_transfer_error();
                        if (recovery_result != ESP_OK) {
                            ESP_LOGE(TAG, "Transfer error recovery failed after %lu attempts",
                                    (unsigned long)s_usb_state.transfer_retry_count);
                            
                            // If recovery fails, try device reconnection
                            if (s_usb_state.reconnect_attempts < USB_RECONNECT_MAX_ATTEMPTS) {
                                ESP_LOGI(TAG, "Attempting device reconnection");
                                usb_out_attempt_reconnection();
                            }
                        }
                        break;
                        
                    default:
                        ESP_LOGD(TAG, "Unhandled UAC device event: %d", event);
                        break;
                }
            } else if (APP_EVENT == evt_queue.event_group) {
                ESP_LOGI(TAG, "App event received, exiting UAC task");
                break;
            }
        }
    }

    // Cleanup: Close any open devices
    if (s_usb_state.spk_dev_handle != NULL) {
        ESP_LOGI(TAG, "Closing speaker device");
        uac_host_device_close(s_usb_state.spk_dev_handle);
        s_usb_state.spk_dev_handle = NULL;
        s_spk_dev_handle = NULL;  // Update global handle for web_server.c
    }
    
    ESP_LOGI(TAG, "Uninstalling UAC driver");
    ESP_ERROR_CHECK(uac_host_uninstall());
    vTaskDelete(NULL);
}

esp_err_t usb_out_init(void) {
    if (s_usb_state.initialized) {
        ESP_LOGW(TAG, "USB output already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing USB output subsystem");
    
    // Create event queue for USB/UAC events
    s_usb_state.event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(s_event_queue_t));
    if (s_usb_state.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize state
    s_usb_state.spk_dev_handle = NULL;
    s_spk_dev_handle = NULL;  // Initialize global handle for web_server.c
    s_usb_state.usb_host_task_handle = NULL;
    s_usb_state.uac_task_handle = NULL;
    s_usb_state.usb_host_running = false;
    s_usb_state.initialized = true;
    s_usb_state.device_enumeration_complete = false;
    s_usb_state.enumeration_start_time = 0;
    usb_out_reset_error_counters();
    
    // Wait for device enumeration with timeout
    ESP_LOGI(TAG, "Waiting for USB device enumeration (timeout: %d ms)", USB_ENUMERATION_TIMEOUT_MS);
    s_usb_state.enumeration_start_time = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "USB output subsystem initialized successfully");
    return ESP_OK;
}

esp_err_t usb_out_start(void) {
    if (!s_usb_state.initialized) {
        ESP_LOGE(TAG, "USB output not initialized, call usb_out_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_usb_state.usb_host_running) {
        ESP_LOGW(TAG, "USB output already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting USB output tasks");
    
    esp_err_t err = ESP_OK;
    int init_retry = 0;
    
    // Retry USB host initialization if it fails
    while (init_retry < USB_INIT_MAX_RETRIES) {
        s_usb_state.usb_host_running = true;
        
        // Create UAC library task
        BaseType_t ret = xTaskCreatePinnedToCore(
            uac_lib_task,
            "uac_events",
            UAC_TASK_STACK_SIZE,
            NULL,
            USER_TASK_PRIORITY,
            &s_usb_state.uac_task_handle,
            0
        );
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create UAC task (attempt %d/%d)",
                     init_retry + 1, USB_INIT_MAX_RETRIES);
            s_usb_state.usb_host_running = false;
            err = ESP_ERR_NO_MEM;
            
            // Clean up and retry
            init_retry++;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Create USB host library task
        ret = xTaskCreatePinnedToCore(
            usb_lib_task,
            "usb_events",
            USB_HOST_TASK_STACK_SIZE,
            (void *)s_usb_state.uac_task_handle,
            USB_HOST_TASK_PRIORITY,
            &s_usb_state.usb_host_task_handle,
            0
        );
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create USB host task (attempt %d/%d)",
                     init_retry + 1, USB_INIT_MAX_RETRIES);
            s_usb_state.usb_host_running = false;
            err = ESP_ERR_NO_MEM;
            
            // Clean up UAC task
            if (s_usb_state.uac_task_handle) {
                vTaskDelete(s_usb_state.uac_task_handle);
                s_usb_state.uac_task_handle = NULL;
            }
            
            // Retry
            init_retry++;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        
        // Success
        ESP_LOGI(TAG, "USB output started successfully");
        return ESP_OK;
    }
    
    // All retries failed
    ESP_LOGE(TAG, "Failed to start USB output after %d attempts", USB_INIT_MAX_RETRIES);
    
    // Ensure proper cleanup
    if (s_usb_state.uac_task_handle) {
        vTaskDelete(s_usb_state.uac_task_handle);
        s_usb_state.uac_task_handle = NULL;
    }
    if (s_usb_state.usb_host_task_handle) {
        vTaskDelete(s_usb_state.usb_host_task_handle);
        s_usb_state.usb_host_task_handle = NULL;
    }
    
    return err == ESP_OK ? ESP_FAIL : err;
}

esp_err_t usb_out_stop(void) {
    if (!s_usb_state.initialized) {
        ESP_LOGW(TAG, "USB output not initialized");
        return ESP_OK;
    }
    
    if (!s_usb_state.usb_host_running) {
        ESP_LOGW(TAG, "USB output not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping USB output");
    
    s_usb_state.usb_host_running = false;
    
    // Send stop event to UAC task
    if (s_usb_state.event_queue) {
        s_event_queue_t evt_queue = { .event_group = APP_EVENT };
        xQueueSend(s_usb_state.event_queue, &evt_queue, 0);
    }
    
    // Wait for tasks to complete (with timeout)
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Tasks should delete themselves, but we'll clear the handles
    s_usb_state.usb_host_task_handle = NULL;
    s_usb_state.uac_task_handle = NULL;
    
    ESP_LOGI(TAG, "USB output stopped");
    return ESP_OK;
}

uac_host_device_handle_t usb_out_get_device_handle(void) {
    return s_usb_state.spk_dev_handle;
}

bool usb_out_is_connected(void) {
    return s_usb_state.spk_dev_handle != NULL;
}

esp_err_t usb_out_deinit(void) {
    if (!s_usb_state.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing USB output");
    
    // Stop if running
    if (s_usb_state.usb_host_running) {
        usb_out_stop();
    }
    
    // Delete event queue
    if (s_usb_state.event_queue) {
        vQueueDelete(s_usb_state.event_queue);
        s_usb_state.event_queue = NULL;
    }
    
    s_usb_state.initialized = false;
    
    ESP_LOGI(TAG, "USB output deinitialized");
    return ESP_OK;
}

esp_err_t usb_out_set_volume(float volume) {
    if (s_usb_state.spk_dev_handle == NULL) {
        ESP_LOGW(TAG, "Cannot set volume - no device connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Volume should be in range 0-100
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 100.0f) volume = 100.0f;
    
    // UAC expects volume in percentage * 100 (0-10000)
    float volume_percent = volume * 100.0f;
    
    esp_err_t err = uac_host_device_set_volume(s_usb_state.spk_dev_handle, volume_percent);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Volume set to %.1f%%", volume);
    } else {
        ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t usb_out_get_volume(float *volume) {
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_usb_state.spk_dev_handle == NULL) {
        ESP_LOGW(TAG, "Cannot get volume - no device connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Note: UAC host library doesn't provide a get_volume function
    // Return the last set volume from lifecycle manager
    *volume = lifecycle_get_volume() * 100.0f;
    ESP_LOGD(TAG, "Current volume from config: %.1f%%", *volume);
    
    return ESP_OK;
}

esp_err_t usb_out_write(const uint8_t *data, size_t size, TickType_t timeout) {
    // Input validation
    if (data == NULL || size == 0) {
        ESP_LOGE(TAG, "Invalid write parameters: data=%p, size=%u", data, (unsigned)size);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Bounds checking for buffer size
    if (size > PCM_CHUNK_SIZE * 8) {  // Sanity check for maximum buffer size
        ESP_LOGE(TAG, "Write size too large: %u bytes (max: %u)",
                (unsigned)size, (unsigned)(PCM_CHUNK_SIZE * 8));
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Validate device handle
    if (!usb_out_validate_handle(s_usb_state.spk_dev_handle)) {
        ESP_LOGD(TAG, "Cannot write audio - no device connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Write audio data to the USB device with retry logic
    esp_err_t err = ESP_FAIL;
    int retry_count = 0;
    uint32_t retry_delay = USB_TRANSFER_RETRY_DELAY_MS;
    
    while (retry_count <= USB_TRANSFER_RETRY_COUNT) {
        // Cast away const qualifier (UAC API limitation)
        err = uac_host_device_write(s_usb_state.spk_dev_handle, (uint8_t *)data, size, timeout);
        
        if (err == ESP_OK || err == ESP_ERR_TIMEOUT) {
            // Success or timeout (not a critical error)
            if (retry_count > 0) {
                ESP_LOGI(TAG, "USB write succeeded after %d retries", retry_count);
                s_usb_state.transfer_retry_count = 0;  // Reset retry counter on success
            }
            break;
        }
        
        // Transfer failed
        ESP_LOGW(TAG, "USB audio write failed (attempt %d/%d): %s",
                retry_count + 1, USB_TRANSFER_RETRY_COUNT + 1, esp_err_to_name(err));
        
        if (retry_count < USB_TRANSFER_RETRY_COUNT) {
            // Apply exponential backoff
            vTaskDelay(pdMS_TO_TICKS(retry_delay));
            retry_delay *= 2;  // Double the delay for next retry
            retry_count++;
            s_usb_state.transfer_retry_count++;
        } else {
            // All retries exhausted
            ESP_LOGE(TAG, "USB write failed after all retries");
            s_usb_state.transfer_error_count++;
            break;
        }
    }
    
    return err;
}

esp_err_t usb_out_stop_playback(void) {
    if (s_usb_state.spk_dev_handle == NULL) {
        ESP_LOGW(TAG, "Cannot stop playback - no device connected");
        return ESP_OK; // Not an error if already stopped
    }
    
    ESP_LOGI(TAG, "Stopping USB playback");
    
    esp_err_t err = uac_host_device_stop(s_usb_state.spk_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop USB device: %s", esp_err_to_name(err));
    }
    
    return err;
}

// Save device parameters for reconnection after sleep
static esp_err_t usb_out_save_device_params(uint8_t addr, uint8_t iface_num, const uac_host_stream_config_t *stream_config) {
    if (stream_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving USB device parameters: addr=%d, iface=%d, SR=%lu, BD=%d, CH=%d",
             addr, iface_num,
             (unsigned long)stream_config->sample_freq,
             stream_config->bit_resolution,
             stream_config->channels);
    
    s_usb_state.saved_device.addr = addr;
    s_usb_state.saved_device.iface_num = iface_num;
    memcpy(&s_usb_state.saved_device.stream_config, stream_config, sizeof(uac_host_stream_config_t));
    s_usb_state.saved_device.valid = true;
    
    return ESP_OK;
}

// Restore device using saved parameters (for fast reconnection after sleep)
static esp_err_t usb_out_restore_device(void) {
    if (!s_usb_state.saved_device.valid) {
        ESP_LOGW(TAG, "No saved device parameters available");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_usb_state.spk_dev_handle != NULL) {
        ESP_LOGW(TAG, "Device already connected, cannot restore");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Attempting to restore USB device with saved parameters");
    ESP_LOGI(TAG, "  addr=%d, iface=%d, SR=%lu, BD=%d, CH=%d",
             s_usb_state.saved_device.addr,
             s_usb_state.saved_device.iface_num,
             (unsigned long)s_usb_state.saved_device.stream_config.sample_freq,
             s_usb_state.saved_device.stream_config.bit_resolution,
             s_usb_state.saved_device.stream_config.channels);
    
    // Configure device with saved parameters
    const uac_host_device_config_t dev_config = {
        .addr = s_usb_state.saved_device.addr,
        .iface_num = s_usb_state.saved_device.iface_num,
        .buffer_size = PCM_CHUNK_SIZE * 4,
        .buffer_threshold = 0,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    
    uac_host_device_handle_t uac_device_handle = NULL;
    esp_err_t err = uac_host_device_open(&dev_config, &uac_device_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open device with saved parameters: %s", esp_err_to_name(err));
        return err;
    }
    
    // Start the device with saved stream configuration
    err = uac_host_device_start(uac_device_handle, &s_usb_state.saved_device.stream_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start device with saved parameters: %s", esp_err_to_name(err));
        uac_host_device_close(uac_device_handle);
        return err;
    }
    
    // Restore volume from lifecycle manager
    float volume_percent = lifecycle_get_volume() * 100.0f;
    uac_host_device_set_volume(uac_device_handle, volume_percent);
    
    // Save the device handle and start playback
    s_usb_state.spk_dev_handle = uac_device_handle;
    s_spk_dev_handle = uac_device_handle;  // Update global handle for web_server.c
    ESP_LOGI(TAG, "USB device restored successfully");

    // Start playback (this will set the audio_device_handle and playing flag)
    start_playback((audio_device_handle_t)uac_device_handle);
    
    return ESP_OK;
}

// Public function to prepare device for sleep mode
esp_err_t usb_out_prepare_for_sleep(void) {
    if (s_usb_state.spk_dev_handle == NULL) {
        ESP_LOGW(TAG, "No device to prepare for sleep");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Preparing USB device for sleep mode");
    
    // Stop playback
    stop_playback();
    
    // Stop the device but keep it open
    esp_err_t err = uac_host_device_stop(s_usb_state.spk_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop device for sleep: %s", esp_err_to_name(err));
    }
    
    return err;
}

// Public function to restore device after wake from sleep
esp_err_t usb_out_restore_after_wake(void) {
    if (s_usb_state.spk_dev_handle == NULL) {
        // Try to restore using saved parameters
        return usb_out_restore_device();
    }
    
    if (!s_usb_state.saved_device.valid) {
        ESP_LOGW(TAG, "No saved device parameters to restore");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Restoring USB device after wake");
    
    // Restart the device with saved parameters
    esp_err_t err = uac_host_device_start(s_usb_state.spk_dev_handle, &s_usb_state.saved_device.stream_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart device after wake: %s", esp_err_to_name(err));
        return err;
    }
    
    // Restore volume from lifecycle manager
    float volume_percent = lifecycle_get_volume() * 100.0f;
    uac_host_device_set_volume(s_usb_state.spk_dev_handle, volume_percent);
    
    // Resume playback
    resume_playback();
    
    ESP_LOGI(TAG, "USB device restored after wake");
    return ESP_OK;
}

// Error handling helper functions

static esp_err_t usb_out_handle_transfer_error(void) {
    // Check if we've exceeded the retry limit
    if (s_usb_state.transfer_retry_count >= USB_TRANSFER_RETRY_COUNT) {
        ESP_LOGE(TAG, "Transfer error retry limit exceeded");
        return ESP_FAIL;
    }
    
    // Calculate exponential backoff delay
    uint32_t backoff_delay = USB_TRANSFER_RETRY_DELAY_MS * (1 << s_usb_state.transfer_retry_count);
    ESP_LOGI(TAG, "Handling transfer error with %lu ms backoff delay", (unsigned long)backoff_delay);
    
    // Wait before retry
    vTaskDelay(pdMS_TO_TICKS(backoff_delay));
    
    // Validate device is still connected
    if (!usb_out_validate_handle(s_usb_state.spk_dev_handle)) {
        ESP_LOGE(TAG, "Device disconnected during error recovery");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Attempt to recover by restarting the stream
    if (s_usb_state.saved_device.valid) {
        ESP_LOGI(TAG, "Attempting to restart stream for error recovery");
        esp_err_t err = uac_host_device_stop(s_usb_state.spk_dev_handle);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));  // Brief pause
            err = uac_host_device_start(s_usb_state.spk_dev_handle,
                                       &s_usb_state.saved_device.stream_config);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Stream restarted successfully");
                s_usb_state.transfer_retry_count = 0;
                return ESP_OK;
            }
        }
        ESP_LOGE(TAG, "Failed to restart stream: %s", esp_err_to_name(err));
    }
    
    s_usb_state.transfer_retry_count++;
    return ESP_FAIL;
}

static esp_err_t usb_out_attempt_reconnection(void) {
    ESP_LOGI(TAG, "Attempting USB device reconnection (attempt %lu/%d)",
             (unsigned long)(s_usb_state.reconnect_attempts + 1),
             USB_RECONNECT_MAX_ATTEMPTS);
    
    // Check if we have saved device parameters
    if (!s_usb_state.saved_device.valid) {
        ESP_LOGW(TAG, "No saved device parameters for reconnection");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check reconnection attempt limit
    if (s_usb_state.reconnect_attempts >= USB_RECONNECT_MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "Maximum reconnection attempts reached");
        return ESP_FAIL;
    }
    
    // Apply reconnection delay with exponential backoff
    TickType_t current_time = xTaskGetTickCount();
    TickType_t time_since_last = current_time - s_usb_state.last_reconnect_time;
    uint32_t required_delay = USB_RECONNECT_DELAY_MS * (1 << s_usb_state.reconnect_attempts);
    
    if (time_since_last < pdMS_TO_TICKS(required_delay)) {
        vTaskDelay(pdMS_TO_TICKS(required_delay) - time_since_last);
    }
    
    s_usb_state.last_reconnect_time = xTaskGetTickCount();
    s_usb_state.reconnect_attempts++;
    
    // Try to restore the device
    esp_err_t err = usb_out_restore_device();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device reconnected successfully");
        s_usb_state.reconnect_attempts = 0;  // Reset counter on success
        usb_out_reset_error_counters();
    } else {
        ESP_LOGE(TAG, "Reconnection attempt failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

static bool usb_out_validate_handle(uac_host_device_handle_t handle) {
    // Basic validation - check if handle is NULL
    if (handle == NULL) {
        return false;
    }
    
    // Check if this is our current device handle
    if (handle != s_usb_state.spk_dev_handle) {
        ESP_LOGW(TAG, "Handle mismatch: expected %p, got %p",
                s_usb_state.spk_dev_handle, handle);
        return false;
    }
    
    // Check if USB subsystem is initialized and running
    if (!s_usb_state.initialized || !s_usb_state.usb_host_running) {
        ESP_LOGW(TAG, "USB subsystem not initialized or not running");
        return false;
    }
    
    return true;
}

static void usb_out_reset_error_counters(void) {
    s_usb_state.transfer_error_count = 0;
    s_usb_state.transfer_retry_count = 0;
    s_usb_state.last_error_time = 0;
}

// Check for device enumeration timeout
esp_err_t usb_out_check_enumeration_timeout(void) {
    if (s_usb_state.device_enumeration_complete) {
        return ESP_OK;
    }
    
    if (s_usb_state.enumeration_start_time == 0) {
        s_usb_state.enumeration_start_time = xTaskGetTickCount();
        return ESP_OK;
    }
    
    TickType_t current_time = xTaskGetTickCount();
    TickType_t elapsed = current_time - s_usb_state.enumeration_start_time;
    
    if (elapsed > pdMS_TO_TICKS(USB_ENUMERATION_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "USB device enumeration timeout after %d ms", USB_ENUMERATION_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_ERR_NOT_FOUND;  // Still waiting
}

#else // IS_USB

#define TAG "usb_out"

esp_err_t usb_out_init(void) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

esp_err_t usb_out_start(void) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

esp_err_t usb_out_stop(void) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

uac_host_device_handle_t usb_out_get_device_handle(void) {
    return NULL;
}

bool usb_out_is_connected(void) {
    return false;
}

esp_err_t usb_out_deinit(void) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

esp_err_t usb_out_set_volume(float volume) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

esp_err_t usb_out_get_volume(float *volume) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    if (volume) *volume = 0.0f;
    return ESP_OK;
}

esp_err_t usb_out_write(const uint8_t *data, size_t size, TickType_t timeout) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

esp_err_t usb_out_stop_playback(void) {
    ESP_LOGI(TAG, "USB support is not enabled in this build.");
    return ESP_OK;
}

#endif // IS_USB