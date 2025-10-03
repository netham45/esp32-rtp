#include "state_machine.h"
#include "lifecycle_internal.h"
#include "hw_init.h"
#include "services.h"
#include "modes.h"
#include "sleep.h"
#include "config.h"
#include "../global.h"
#include "../config/config_manager.h"
#include "../wifi/wifi_manager.h"
#include "../pairing/pairing_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#define LIFECYCLE_EVENT_QUEUE_SIZE 10

#undef TAG
#define TAG "lifecycle_sm"

static QueueHandle_t s_lifecycle_event_queue = NULL;
static lifecycle_state_t s_current_state = LIFECYCLE_STATE_INITIALIZING;

// Forward declarations for state handlers
static void handle_state_initializing(lifecycle_event_t event);
static void handle_state_hw_init(lifecycle_event_t event);
static void handle_state_starting_services(lifecycle_event_t event);
static void handle_state_awaiting_mode_config(lifecycle_event_t event);
static void handle_state_mode_sender_usb(lifecycle_event_t event);
static void handle_state_mode_sender_spdif(lifecycle_event_t event);
static void handle_state_mode_receiver_usb(lifecycle_event_t event);
static void handle_state_mode_receiver_spdif(lifecycle_event_t event);
static void handle_state_sleeping(lifecycle_event_t event);
static void handle_state_error(lifecycle_event_t event);
static void handle_state_pairing(lifecycle_event_t event);

// Forward declarations for state transition helpers
static void set_state(lifecycle_state_t new_state);
static void handle_state_entry(lifecycle_state_t state);
static void handle_state_exit(lifecycle_state_t state);
static void evaluate_and_transition(void);

/**
 * State entry handler - called when entering a new state
 */
static void handle_state_entry(lifecycle_state_t state) {
    ESP_LOGI(TAG, "LIFECYCLE: Entering state %d", state);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    
    switch (state) {
        case LIFECYCLE_STATE_HW_INIT: {
            // Use extracted hardware initialization functions
            ESP_ERROR_CHECK(lifecycle_hw_init_nvs());
            ESP_ERROR_CHECK(lifecycle_hw_init_config());
            
            // Non-critical initializations
            lifecycle_hw_init_ota();  // Continue on failure
            lifecycle_hw_init_battery();  // Continue on failure
            lifecycle_hw_init_power_management();  // Continue on failure
            
            // DAC detection logic
            lifecycle_hw_init_dac_detection();
            
            // Proceed to starting services
            set_state(LIFECYCLE_STATE_STARTING_SERVICES);
            break;
        }
        case LIFECYCLE_STATE_STARTING_SERVICES: {
            // Use extracted service initialization functions
            ESP_ERROR_CHECK(lifecycle_services_init_wifi());
            lifecycle_services_init_spdif_receiver();  // Only initializes in SPDIF sender mode
            ESP_ERROR_CHECK(lifecycle_services_init_web_server());
            
            set_state(LIFECYCLE_STATE_AWAITING_MODE_CONFIG);
            break;
        }
        case LIFECYCLE_STATE_AWAITING_MODE_CONFIG: {
            // Don't block the state machine task with delays
            // Instead, immediately check if we can transition
            ESP_LOGI(TAG, "Entered AWAITING_MODE_CONFIG, checking for immediate transition");
            
            // For sender modes that don't require WiFi, transition immediately
            // For receiver modes, wait for WiFi connection event
            evaluate_and_transition();
            break;
        }
        case LIFECYCLE_STATE_MODE_SENDER_USB:
        case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
        case LIFECYCLE_STATE_MODE_RECEIVER_USB:
        case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
            lifecycle_mode_start(state);
            break;
        case LIFECYCLE_STATE_SLEEPING:
            lifecycle_sleep_enter_silence_mode();
            break;
        case LIFECYCLE_STATE_PAIRING: {
            ESP_LOGI(TAG, "Starting pairing mode");
            esp_err_t ret = pairing_manager_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start pairing mode: %s", esp_err_to_name(ret));
                // Return to previous state
                evaluate_and_transition();
            }
            break;
        }
        default:
            // No entry action for other states
            break;
    }
}

/**
 * State exit handler - called when leaving a state
 */
static void handle_state_exit(lifecycle_state_t state) {
    ESP_LOGI(TAG, "LIFECYCLE: Exiting state %d", state);
    
    switch (state) {
        case LIFECYCLE_STATE_MODE_SENDER_USB:
        case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
        case LIFECYCLE_STATE_MODE_RECEIVER_USB:
        case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
            lifecycle_mode_stop(state);
            break;
        case LIFECYCLE_STATE_SLEEPING:
            lifecycle_sleep_exit_silence_mode();
            break;
        case LIFECYCLE_STATE_PAIRING:
            ESP_LOGI(TAG, "Exiting pairing mode");
            // Pairing manager cleanup is handled internally
            break;
        default:
            // No exit action for other states
            break;
    }
}

/**
 * Set state with entry/exit handlers
 */
static void set_state(lifecycle_state_t new_state) {
    if (s_current_state == new_state) {
        return;
    }
    handle_state_exit(s_current_state);
    ESP_LOGI(TAG, "LIFECYCLE: Transitioning from state %d to %d", s_current_state, new_state);
    s_current_state = new_state;
    handle_state_entry(s_current_state);
}

/**
 * Evaluate configuration and transition to appropriate mode
 */
static void evaluate_and_transition(void) {
    app_config_t *config = config_manager_get_config();
    
    // Log the current configuration
    ESP_LOGI(TAG, "========== CURRENT CONFIGURATION ==========");
    ESP_LOGI(TAG, "Device mode: %d", config->device_mode);
    ESP_LOGI(TAG, "Sample rate: %d Hz", config->sample_rate);
    ESP_LOGI(TAG, "Bit depth: %d bits", config->bit_depth);
    ESP_LOGI(TAG, "Volume: %.2f", config->volume);
    ESP_LOGI(TAG, "Sender destination IP: %s", config->sender_destination_ip);
    ESP_LOGI(TAG, "Sender destination port: %d", config->sender_destination_port);
    ESP_LOGI(TAG, "S/PDIF data pin: %d", config->spdif_data_pin);
    ESP_LOGI(TAG, "==========================================");
    
    // Check WiFi state first
    wifi_manager_state_t wifi_state = wifi_manager_get_state();
    ESP_LOGI(TAG, "Current WiFi state: %d", wifi_state);
    
    // Only transition to operational modes if WiFi is connected or we're in sender mode
    if (wifi_state != WIFI_MANAGER_STATE_CONNECTED &&
        (config->device_mode == MODE_RECEIVER_USB || config->device_mode == MODE_RECEIVER_SPDIF)) {
        ESP_LOGI(TAG, "WiFi not connected and in receiver mode, staying in AWAITING_MODE_CONFIG");
        return;
    }

    // Determine which state to enter based on device mode
    switch (config->device_mode) {
        case MODE_SENDER_USB:
            ESP_LOGI(TAG, "Transitioning to USB sender mode");
            set_state(LIFECYCLE_STATE_MODE_SENDER_USB);
            break;
        case MODE_SENDER_SPDIF:
            ESP_LOGI(TAG, "Transitioning to S/PDIF sender mode");
            set_state(LIFECYCLE_STATE_MODE_SENDER_SPDIF);
            break;
        case MODE_RECEIVER_USB:
            ESP_LOGI(TAG, "Transitioning to USB receiver mode");
            set_state(LIFECYCLE_STATE_MODE_RECEIVER_USB);
            break;
        case MODE_RECEIVER_SPDIF:
            ESP_LOGI(TAG, "Transitioning to S/PDIF receiver mode");
            set_state(LIFECYCLE_STATE_MODE_RECEIVER_SPDIF);
            break;
    }
}

/**
 * State event handlers
 */

static void handle_state_initializing(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: INITIALIZING");
}

static void handle_state_hw_init(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: HW_INIT");
    // Only process specific events, not all events
    if (event != LIFECYCLE_EVENT_WIFI_CONNECTED &&
        event != LIFECYCLE_EVENT_USB_DAC_CONNECTED &&
        event != LIFECYCLE_EVENT_USB_DAC_DISCONNECTED &&
        event != LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        // Ignore other events while in HW_INIT
        return;
    }
}

static void handle_state_starting_services(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: STARTING_SERVICES");
    // Entry actions are handled in handle_state_entry
}

static void handle_state_awaiting_mode_config(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: AWAITING_MODE_CONFIG");
    switch (event) {
        case LIFECYCLE_EVENT_WIFI_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected, starting services.");
            lifecycle_services_init_mdns();
            evaluate_and_transition();
            break;
        case LIFECYCLE_EVENT_WIFI_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi disconnected.");
            // mDNS continues running and will re-advertise when network returns
            break;
        case LIFECYCLE_EVENT_CONFIGURATION_CHANGED: {
            ESP_LOGI(TAG, "Configuration changed, re-evaluating mode.");
            
            // Check if we now have WiFi credentials
            if (!wifi_manager_has_credentials()) {
                ESP_LOGI(TAG, "No WiFi credentials, staying in AP mode");
                // Stay in AP mode for configuration
            } else {
                ESP_LOGI(TAG, "WiFi credentials available, attempting connection");
                // Try to connect with new credentials
                wifi_manager_state_t state = wifi_manager_get_state();
                if (state == WIFI_MANAGER_STATE_AP_MODE || state == WIFI_MANAGER_STATE_CONNECTION_FAILED) {
                    char ssid[WIFI_SSID_MAX_LENGTH + 1];
                    char password[WIFI_PASSWORD_MAX_LENGTH + 1];
                    if (wifi_manager_get_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
                        wifi_manager_connect(ssid, password);
                    }
                }
            }
            evaluate_and_transition();
            break;
        }
        case LIFECYCLE_EVENT_START_PAIRING:
            ESP_LOGI(TAG, "Starting pairing mode from awaiting config state");
            set_state(LIFECYCLE_STATE_PAIRING);
            break;
        default:
            break;
    }
}

static void handle_state_mode_sender_usb(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_SENDER_USB");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        // Use unified configuration handler to apply immediate changes
        bool restart_required = lifecycle_config_handle_configuration_changed();
        
        // Only re-evaluate/restart if necessary
        if (restart_required) {
            ESP_LOGI(TAG, "Configuration changes require restart, re-evaluating state");
            evaluate_and_transition();
        } else {
            ESP_LOGI(TAG, "Configuration changes applied immediately without restart");
        }
    } else if (event == LIFECYCLE_EVENT_START_PAIRING) {
        set_state(LIFECYCLE_STATE_PAIRING);
    }
}

static void handle_state_mode_sender_spdif(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_SENDER_SPDIF");
    if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        // Use unified configuration handler to apply immediate changes
        bool restart_required = lifecycle_config_handle_configuration_changed();
        
        // Only re-evaluate/restart if necessary
        if (restart_required) {
            ESP_LOGI(TAG, "Configuration changes require restart, re-evaluating state");
            evaluate_and_transition();
        } else {
            ESP_LOGI(TAG, "Configuration changes applied immediately without restart");
        }
    } else if (event == LIFECYCLE_EVENT_START_PAIRING) {
        set_state(LIFECYCLE_STATE_PAIRING);
    }
}

static void handle_state_mode_receiver_usb(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_RECEIVER_USB");
    if (event == LIFECYCLE_EVENT_ENTER_SLEEP) {
        set_state(LIFECYCLE_STATE_SLEEPING);
    } else if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        // Use unified configuration handler to apply immediate changes
        bool restart_required = lifecycle_config_handle_configuration_changed();
        
        // Only re-evaluate/restart if necessary
        if (restart_required) {
            ESP_LOGI(TAG, "Configuration changes require restart, re-evaluating state");
            evaluate_and_transition();
        } else {
            ESP_LOGI(TAG, "Configuration changes applied immediately without restart");
        }
    } else if (event == LIFECYCLE_EVENT_START_PAIRING) {
        set_state(LIFECYCLE_STATE_PAIRING);
    } else if (event == LIFECYCLE_EVENT_SAMPLE_RATE_CHANGE) {
        ESP_LOGI(TAG, "Received sample rate change event, re-evaluating mode.");
        evaluate_and_transition();
    } else if (event == LIFECYCLE_EVENT_SAP_STREAM_FOUND) {
        ESP_LOGI(TAG, "SAP stream found event received - network configuration already handled");
    }
}

static void handle_state_mode_receiver_spdif(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_RECEIVER_SPDIF");
    if (event == LIFECYCLE_EVENT_ENTER_SLEEP) {
        set_state(LIFECYCLE_STATE_SLEEPING);
    } else if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        // Use unified configuration handler to apply immediate changes
        bool restart_required = lifecycle_config_handle_configuration_changed();
        
        // Only re-evaluate/restart if necessary
        if (restart_required) {
            ESP_LOGI(TAG, "Configuration changes require restart, re-evaluating state");
            evaluate_and_transition();
        } else {
            ESP_LOGI(TAG, "Configuration changes applied immediately without restart");
        }
    } else if (event == LIFECYCLE_EVENT_START_PAIRING) {
        set_state(LIFECYCLE_STATE_PAIRING);
    } else if (event == LIFECYCLE_EVENT_SAMPLE_RATE_CHANGE) {
        ESP_LOGI(TAG, "Received sample rate change event, re-evaluating mode.");
        evaluate_and_transition();
    } else if (event == LIFECYCLE_EVENT_SAP_STREAM_FOUND) {
        ESP_LOGI(TAG, "SAP stream found event received - network configuration already handled");
    }
}

static void handle_state_sleeping(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: SLEEPING");
    if (event == LIFECYCLE_EVENT_WAKE_UP) {
        set_state(LIFECYCLE_STATE_AWAITING_MODE_CONFIG);
    }
}

static void handle_state_error(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: ERROR");
}

static void handle_state_pairing(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: PAIRING");
    switch (event) {
        case LIFECYCLE_EVENT_PAIRING_COMPLETE:
            ESP_LOGI(TAG, "Pairing complete, returning to previous mode");
            evaluate_and_transition();
            break;
        case LIFECYCLE_EVENT_CANCEL_PAIRING:
            ESP_LOGI(TAG, "Pairing cancelled by user");
            evaluate_and_transition();
            break;
        case LIFECYCLE_EVENT_WIFI_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected during pairing, aborting");
            evaluate_and_transition();
            break;
        default:
            break;
    }
}

/**
 * Main lifecycle manager task
 */
static void lifecycle_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "Lifecycle manager task started.");
    
    // Initial state transition
    set_state(LIFECYCLE_STATE_HW_INIT);

    while (1) {
        lifecycle_event_t event;
        if (xQueueReceive(s_lifecycle_event_queue, &event, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "LIFECYCLE: Received event %d in state %d", event, s_current_state);
            switch (s_current_state) {
                case LIFECYCLE_STATE_INITIALIZING:
                    handle_state_initializing(event);
                    break;
                case LIFECYCLE_STATE_HW_INIT:
                    handle_state_hw_init(event);
                    break;
                case LIFECYCLE_STATE_STARTING_SERVICES:
                    handle_state_starting_services(event);
                    break;
                case LIFECYCLE_STATE_AWAITING_MODE_CONFIG:
                    handle_state_awaiting_mode_config(event);
                    break;
                case LIFECYCLE_STATE_MODE_SENDER_USB:
                    handle_state_mode_sender_usb(event);
                    break;
                case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
                    handle_state_mode_sender_spdif(event);
                    break;
                case LIFECYCLE_STATE_MODE_RECEIVER_USB:
                    handle_state_mode_receiver_usb(event);
                    break;
                case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
                    handle_state_mode_receiver_spdif(event);
                    break;
                case LIFECYCLE_STATE_SLEEPING:
                    handle_state_sleeping(event);
                    break;
                case LIFECYCLE_STATE_ERROR:
                    handle_state_error(event);
                    break;
                case LIFECYCLE_STATE_PAIRING:
                    handle_state_pairing(event);
                    break;
            }
        }
    }
}

/**
 * Public API implementations
 */

esp_err_t lifecycle_state_machine_init(void) {
    s_lifecycle_event_queue = xQueueCreate(LIFECYCLE_EVENT_QUEUE_SIZE, sizeof(lifecycle_event_t));
    if (s_lifecycle_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create lifecycle event queue");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(lifecycle_manager_task, "lifecycle_mgr", 8192, NULL, 5, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create lifecycle manager task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t lifecycle_state_machine_post_event(lifecycle_event_t event) {
    if (s_lifecycle_event_queue == NULL) {
        ESP_LOGE(TAG, "Lifecycle event queue not initialized");
        return ESP_FAIL;
    }

    if (xQueueSend(s_lifecycle_event_queue, &event, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to post event to lifecycle queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

lifecycle_state_t lifecycle_state_machine_get_current_state(void) {
    return s_current_state;
}