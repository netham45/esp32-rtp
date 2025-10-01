#include "modes.h"
#include "lifecycle_internal.h"
#include "../global.h"
#include "../receiver/audio_out.h"
#include "../receiver/buffer.h"
#include "../receiver/network_in.h"
#include "../receiver/sap_listener.h"
#include "../sender/network_out.h"
#include "../visualizer/visualizer_task.h"
#include "../config/config_manager.h"
#include "esp_log.h"

#ifdef IS_USB
#include "../sender/usb_in.h"
#include "../receiver/usb_out.h"
#endif

#ifdef IS_SPDIF
#include "../sender/spdif_in/spdif_in.h"
#include "../receiver/spdif_out.h"
#endif

#undef TAG
#define TAG "lifecycle_modes"

// Forward declarations for mode-specific functions
static esp_err_t start_mode_sender_usb(void);
static esp_err_t stop_mode_sender_usb(void);
static esp_err_t start_mode_sender_spdif(void);
static esp_err_t stop_mode_sender_spdif(void);
static esp_err_t start_mode_receiver_usb(void);
static esp_err_t stop_mode_receiver_usb(void);
static esp_err_t start_mode_receiver_spdif(void);
static esp_err_t stop_mode_receiver_spdif(void);

/**
 * Start the specified operational mode
 */
esp_err_t lifecycle_mode_start(lifecycle_state_t mode) {
    switch (mode) {
        case LIFECYCLE_STATE_MODE_SENDER_USB:
            return start_mode_sender_usb();
        case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
            return start_mode_sender_spdif();
        case LIFECYCLE_STATE_MODE_RECEIVER_USB:
            return start_mode_receiver_usb();
        case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
            return start_mode_receiver_spdif();
        default:
            ESP_LOGW(TAG, "No start function for state %d", mode);
            return ESP_ERR_INVALID_ARG;
    }
}

/**
 * Stop the specified operational mode
 */
esp_err_t lifecycle_mode_stop(lifecycle_state_t mode) {
    switch (mode) {
        case LIFECYCLE_STATE_MODE_SENDER_USB:
            return stop_mode_sender_usb();
        case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
            return stop_mode_sender_spdif();
        case LIFECYCLE_STATE_MODE_RECEIVER_USB:
            return stop_mode_receiver_usb();
        case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
            return stop_mode_receiver_spdif();
        default:
            ESP_LOGW(TAG, "No stop function for state %d", mode);
            return ESP_ERR_INVALID_ARG;
    }
}

// ==================== USB Sender Mode ====================

static esp_err_t start_mode_sender_usb(void) {
    ESP_LOGI(TAG, "Starting USB sender mode...");
    #ifdef IS_USB
    // Initialize network sender first (reads from pcm_buffer)
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t ret = rtp_sender_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize RTP sender: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize USB input (receives audio from host, writes to pcm_buffer)
    ESP_LOGI(TAG, "Initializing USB audio input (USB speaker device)");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ret = usb_in_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB input: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize visualizer for audio visualization
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ret = visualizer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize visualizer: %s", esp_err_to_name(ret));
        // Non-critical, continue
    } else {
        ESP_LOGI(TAG, "Visualizer initialized successfully");
    }
    
    // Start USB input to begin receiving audio from host
    ESP_LOGI(TAG, "Starting USB audio input");
    ret = usb_in_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB input: %s", esp_err_to_name(ret));
        usb_in_deinit();
        return ret;
    }
    
    // Start network sender to transmit over RTP
    ESP_LOGI(TAG, "Starting RTP network sender");
    ret = rtp_sender_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RTP sender: %s", esp_err_to_name(ret));
        usb_in_stop();
        usb_in_deinit();
        return ret;
    }
    
    ESP_LOGI(TAG, "USB sender mode started successfully (USB audio -> RTP)");
    return ESP_OK;
    #else
    ESP_LOGW(TAG, "USB support not enabled in build");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

static esp_err_t stop_mode_sender_usb(void) {
    ESP_LOGI(TAG, "Stopping USB sender mode...");
    #ifdef IS_USB
    // Stop RTP sender first
    esp_err_t ret = visualizer_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop visualizer: %s", esp_err_to_name(ret));
    }

    ret = rtp_sender_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop RTP sender: %s", esp_err_to_name(ret));
    }
    
    // Stop USB input
    ret = usb_in_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop USB input: %s", esp_err_to_name(ret));
    }
    
    // Deinitialize USB input
    usb_in_deinit();
    
    ESP_LOGI(TAG, "USB sender mode stopped");
    return ESP_OK;
    #else
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

// ==================== SPDIF Sender Mode ====================

static esp_err_t start_mode_sender_spdif(void) {
    ESP_LOGI(TAG, "Starting S/PDIF sender mode...");
    #ifdef IS_SPDIF
    ESP_LOGI(TAG, "Initializing Scream sender");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t ret = rtp_sender_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Scream sender: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = visualizer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize visualizer: %s", esp_err_to_name(ret));
        // Non-critical, continue
    } else {
        ESP_LOGI(TAG, "Visualizer initialized successfully");
    }
    
    ESP_LOGI(TAG, "Starting S/PDIF receiver");
    ret = spdif_receiver_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start S/PDIF receiver: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Starting RTP sender");
    ret = rtp_sender_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RTP sender: %s", esp_err_to_name(ret));
        spdif_receiver_stop();
        return ret;
    }
    ESP_LOGI(TAG, "S/PDIF sender mode started successfully");
    return ESP_OK;
    #else
    ESP_LOGW(TAG, "S/PDIF support not enabled in build");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

static esp_err_t stop_mode_sender_spdif(void) {
    ESP_LOGI(TAG, "Stopping S/PDIF sender mode...");
    #ifdef IS_SPDIF
    esp_err_t ret = rtp_sender_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop Scream sender: %s", esp_err_to_name(ret));
    }
    ret = spdif_receiver_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop S/PDIF receiver: %s", esp_err_to_name(ret));
    }
    ret = visualizer_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop visualizer: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
    #else
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

// ==================== USB Receiver Mode ====================

static esp_err_t start_mode_receiver_usb(void) {
    ESP_LOGI(TAG, "Starting USB receiver mode...");
    #ifdef IS_USB
    // Setup audio output first
    setup_audio();

    // Setup buffer for network->USB streaming
    setup_buffer();

    // Setup network receiver
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    network_init();

    // Initialize and start SAP listener
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t ret = sap_listener_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SAP listener: %s", esp_err_to_name(ret));
    } else {
        ret = sap_listener_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start SAP listener: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "SAP listener started successfully");
        }
    }

    // Initialize USB host subsystem
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ret = usb_out_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB host: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start USB host for DAC output
    ret = usb_out_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB host: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize visualizer for audio visualization
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ret = visualizer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize visualizer: %s", esp_err_to_name(ret));
        // Non-critical, continue
    } else {
        ESP_LOGI(TAG, "Visualizer initialized successfully");
    }
    
    ESP_LOGI(TAG, "USB receiver mode started successfully");
    return ESP_OK;
    #else
    ESP_LOGW(TAG, "USB support not enabled in build");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

static esp_err_t stop_mode_receiver_usb(void) {
    ESP_LOGI(TAG, "Stopping USB receiver mode...");
    #ifdef IS_USB
    
    // Stop visualizer first
    esp_err_t ret = visualizer_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop visualizer: %s", esp_err_to_name(ret));
    }
    
    ret = usb_out_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop USB host: %s", esp_err_to_name(ret));
    }

    ret = usb_out_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize USB host: %s", esp_err_to_name(ret));
    }

    // Stop SAP listener
    ret = sap_listener_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop SAP listener: %s", esp_err_to_name(ret));
    }
    
    ret = sap_listener_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize SAP listener: %s", esp_err_to_name(ret));
    }

    // TODO: Stop network and audio tasks properly
    // For now, these don't have clean stop functions
    return ESP_OK;
    #else
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

// ==================== SPDIF Receiver Mode ====================

static esp_err_t start_mode_receiver_spdif(void) {
    ESP_LOGI(TAG, "Starting S/PDIF receiver mode...");
    uint32_t sample_rate = lifecycle_get_sample_rate();
    uint8_t spdif_data_pin = lifecycle_get_spdif_data_pin();

    // Setup buffer for network->S/PDIF streaming
    setup_buffer();

    setup_audio();

        
    ESP_LOGI(TAG, "Initializing SPDIF output with pin %d and sample rate %lu",
        spdif_data_pin, sample_rate);
        
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t err = spdif_init(sample_rate);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPDIF initialized successfully");
    } else {
        ESP_LOGE(TAG, "Failed to initialize SPDIF: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Audio output will not be available. Please check the SPDIF pin configuration in the web UI.");
    }

    
    // Setup network receiver
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    network_init();
    
    // Initialize and start SAP listener
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    esp_err_t ret = sap_listener_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SAP listener: %s", esp_err_to_name(ret));
    } else {
        ret = sap_listener_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start SAP listener: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "SAP listener started successfully");
        }
    }
    
    // Initialize visualizer for audio visualization
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ret = visualizer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize visualizer: %s", esp_err_to_name(ret));
        // Non-critical, continue
    } else {
        ESP_LOGI(TAG, "Visualizer initialized successfully");
    }
    
    ESP_LOGI(TAG, "S/PDIF receiver mode started successfully");
    return ESP_OK;
}

static esp_err_t stop_mode_receiver_spdif(void) {
    ESP_LOGI(TAG, "Stopping S/PDIF receiver mode...");
    
    // Stop visualizer first
    esp_err_t ret = visualizer_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop visualizer: %s", esp_err_to_name(ret));
    }
    
    // Stop SAP listener
    ret = sap_listener_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop SAP listener: %s", esp_err_to_name(ret));
    }
    
    ret = sap_listener_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize SAP listener: %s", esp_err_to_name(ret));
    }
    
    #ifdef IS_SPDIF
    // TODO: Stop network, buffer, and S/PDIF output properly
    // For now, these don't have clean stop functions
    #endif
    return ESP_OK;
}