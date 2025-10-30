#include "ota_manager.h"
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_format.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "OTA_MANAGER";

// OTA manager context structure
typedef struct {
    ota_state_t state;
    ota_error_code_t error_code;
    ota_status_t status;
    ota_config_t config;
    esp_ota_handle_t update_handle;
    const esp_partition_t *update_partition;
    const esp_partition_t *running_partition;
    uint32_t bytes_written;
    uint32_t expected_size;
    uint32_t start_time;
    SemaphoreHandle_t mutex;
    void (*progress_callback)(uint8_t, uint32_t, uint32_t);
    void (*state_callback)(ota_state_t, ota_error_code_t);
    uint32_t successful_updates;
    uint32_t failed_updates;
    uint64_t total_bytes_written;
    bool initialized;
} ota_manager_ctx_t;

// Global OTA manager context
static ota_manager_ctx_t *g_ota_ctx = NULL;

// Default configuration
static const ota_config_t default_config = {
    .buffer_size = 4096,
    .timeout_ms = 300000,  // 5 minutes
    .enable_rollback_protection = true,
    .validate_image = true,
    .max_retry_count = 3,
    .watchdog_timeout_ms = 60000  // 1 minute
};

// Helper function to get current time in milliseconds
static uint32_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

// Helper function to set error state
static void set_error_state(ota_error_code_t error_code, const char *message) {
    if (!g_ota_ctx) return;
    
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    g_ota_ctx->state = OTA_STATE_ERROR;
    g_ota_ctx->error_code = error_code;
    g_ota_ctx->status.state = OTA_STATE_ERROR;
    g_ota_ctx->status.error_code = error_code;
    if (message) {
        strncpy(g_ota_ctx->status.error_message, message, sizeof(g_ota_ctx->status.error_message) - 1);
        g_ota_ctx->status.error_message[sizeof(g_ota_ctx->status.error_message) - 1] = '\0';
    }
    xSemaphoreGive(g_ota_ctx->mutex);
    
    if (g_ota_ctx->state_callback) {
        g_ota_ctx->state_callback(OTA_STATE_ERROR, error_code);
    }
    
    ESP_LOGE(TAG, "OTA Error: %s (code: %d)", message ? message : "Unknown error", error_code);
}

// Helper function to update state
static void update_state(ota_state_t new_state) {
    if (!g_ota_ctx) return;
    
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    g_ota_ctx->state = new_state;
    g_ota_ctx->status.state = new_state;
    xSemaphoreGive(g_ota_ctx->mutex);
    
    if (g_ota_ctx->state_callback) {
        g_ota_ctx->state_callback(new_state, OTA_ERR_NONE);
    }
    
    ESP_LOGI(TAG, "OTA State changed to: %d", new_state);
}

// Helper function to update progress
static void update_progress(void) {
    if (!g_ota_ctx || g_ota_ctx->expected_size == 0) return;
    
    uint8_t percent = (g_ota_ctx->bytes_written * 100) / g_ota_ctx->expected_size;
    
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    g_ota_ctx->status.received_size = g_ota_ctx->bytes_written;
    g_ota_ctx->status.progress_percent = percent;
    g_ota_ctx->status.update_duration_ms = get_time_ms() - g_ota_ctx->start_time;
    xSemaphoreGive(g_ota_ctx->mutex);
    
    if (g_ota_ctx->progress_callback) {
        g_ota_ctx->progress_callback(percent, g_ota_ctx->bytes_written, g_ota_ctx->expected_size);
    }
}

// Initialize OTA manager
esp_err_t ota_manager_init(const ota_config_t *config) {
    if (g_ota_ctx) {
        ESP_LOGW(TAG, "OTA manager already initialized");
        return ESP_OK;
    }
    
    // Allocate context
    g_ota_ctx = (ota_manager_ctx_t *)calloc(1, sizeof(ota_manager_ctx_t));
    if (!g_ota_ctx) {
        ESP_LOGE(TAG, "Failed to allocate OTA context");
        return ESP_ERR_NO_MEM;
    }
    
    // Create mutex
    g_ota_ctx->mutex = xSemaphoreCreateMutex();
    if (!g_ota_ctx->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(g_ota_ctx);
        g_ota_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Set configuration
    if (config) {
        memcpy(&g_ota_ctx->config, config, sizeof(ota_config_t));
    } else {
        memcpy(&g_ota_ctx->config, &default_config, sizeof(ota_config_t));
    }
    
    // Get running partition
    g_ota_ctx->running_partition = esp_ota_get_running_partition();
    if (!g_ota_ctx->running_partition) {
        ESP_LOGE(TAG, "Failed to get running partition");
        vSemaphoreDelete(g_ota_ctx->mutex);
        free(g_ota_ctx);
        g_ota_ctx = NULL;
        return ESP_FAIL;
    }
    
    // Initialize status
    g_ota_ctx->state = OTA_STATE_IDLE;
    g_ota_ctx->error_code = OTA_ERR_NONE;
    g_ota_ctx->status.state = OTA_STATE_IDLE;
    g_ota_ctx->status.error_code = OTA_ERR_NONE;
    g_ota_ctx->initialized = true;
    
    ESP_LOGI(TAG, "OTA manager initialized successfully");
    ESP_LOGI(TAG, "Running partition: %s at offset 0x%x", 
             g_ota_ctx->running_partition->label, 
             g_ota_ctx->running_partition->address);
    
    return ESP_OK;
}

// Deinitialize OTA manager
esp_err_t ota_manager_deinit(void) {
    if (!g_ota_ctx) {
        return ESP_OK;
    }
    
    // Abort any ongoing update
    if (g_ota_ctx->state != OTA_STATE_IDLE) {
        ota_manager_abort();
    }
    
    // Destroy mutex
    if (g_ota_ctx->mutex) {
        vSemaphoreDelete(g_ota_ctx->mutex);
    }
    
    // Free context
    free(g_ota_ctx);
    g_ota_ctx = NULL;
    
    ESP_LOGI(TAG, "OTA manager deinitialized");
    return ESP_OK;
}

// Start OTA update process
esp_err_t ota_manager_start(uint32_t expected_size, const ota_firmware_info_t *firmware_info) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_ota_ctx->state != OTA_STATE_IDLE) {
        ESP_LOGE(TAG, "OTA update already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting OTA update, expected size: %u", expected_size);
    
    // Log partition states for debugging
    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    
    if (ota0) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(ota0, &state) == ESP_OK) {
            ESP_LOGI(TAG, "OTA_0 partition state: %d", state);
        }
    }
    if (ota1) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(ota1, &state) == ESP_OK) {
            ESP_LOGI(TAG, "OTA_1 partition state: %d", state);
        }
    }
    ESP_LOGI(TAG, "Currently running from: %s", g_ota_ctx->running_partition->label);
    
    // Update state
    update_state(OTA_STATE_CHECKING);
    
    // Get next update partition
    g_ota_ctx->update_partition = esp_ota_get_next_update_partition(NULL);
    if (!g_ota_ctx->update_partition) {
        set_error_state(OTA_ERR_PARTITION_NOT_FOUND, "Failed to find update partition");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Update partition: %s at offset 0x%x, size: 0x%x", 
             g_ota_ctx->update_partition->label,
             g_ota_ctx->update_partition->address,
             g_ota_ctx->update_partition->size);
    
    // Check partition size
    if (expected_size > 0 && expected_size > g_ota_ctx->update_partition->size) {
        set_error_state(OTA_ERR_INVALID_SIZE, "Firmware too large for partition");
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Begin OTA update
    esp_err_t err = esp_ota_begin(g_ota_ctx->update_partition, 
                                   expected_size ? expected_size : OTA_SIZE_UNKNOWN, 
                                   &g_ota_ctx->update_handle);
    if (err != ESP_OK) {
        set_error_state(OTA_ERR_WRITE_FAILED, "Failed to begin OTA update");
        return err;
    }
    
    // Initialize update context
    g_ota_ctx->expected_size = expected_size;
    g_ota_ctx->bytes_written = 0;
    g_ota_ctx->start_time = get_time_ms();
    
    // Update status
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    g_ota_ctx->status.total_size = expected_size;
    g_ota_ctx->status.received_size = 0;
    g_ota_ctx->status.progress_percent = 0;
    g_ota_ctx->status.update_start_time = g_ota_ctx->start_time;
    g_ota_ctx->status.update_duration_ms = 0;
    g_ota_ctx->status.error_message[0] = '\0';
    
    if (firmware_info) {
        strncpy(g_ota_ctx->status.new_version, firmware_info->version, 
                sizeof(g_ota_ctx->status.new_version) - 1);
    }
    xSemaphoreGive(g_ota_ctx->mutex);
    
    update_state(OTA_STATE_DOWNLOADING);
    
    ESP_LOGI(TAG, "OTA update started successfully");
    return ESP_OK;
}

// Write data chunk to OTA partition
esp_err_t ota_manager_write(const uint8_t *data, size_t size) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_ota_ctx->state != OTA_STATE_DOWNLOADING) {
        ESP_LOGE(TAG, "Invalid state for write operation");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Invalid data or size");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check for timeout
    uint32_t elapsed = get_time_ms() - g_ota_ctx->start_time;
    if (elapsed > g_ota_ctx->config.timeout_ms) {
        set_error_state(OTA_ERR_TIMEOUT, "OTA update timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    // Write data to partition
    esp_err_t err = esp_ota_write(g_ota_ctx->update_handle, data, size);
    if (err != ESP_OK) {
        set_error_state(OTA_ERR_WRITE_FAILED, "Failed to write OTA data");
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Update progress
    g_ota_ctx->bytes_written += size;
    g_ota_ctx->total_bytes_written += size;
    update_progress();
    
    ESP_LOGD(TAG, "Written %u bytes (total: %u)", size, g_ota_ctx->bytes_written);
    
    return ESP_OK;
}

// Complete OTA update and validate image
esp_err_t ota_manager_complete(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_ota_ctx->state != OTA_STATE_DOWNLOADING) {
        ESP_LOGE(TAG, "Invalid state for complete operation");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Completing OTA update, total bytes written: %u", g_ota_ctx->bytes_written);
    
    update_state(OTA_STATE_VERIFYING);
    
    // End OTA update
    esp_err_t err = esp_ota_end(g_ota_ctx->update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            set_error_state(OTA_ERR_VERIFY_FAILED, "Image validation failed");
        } else {
            set_error_state(OTA_ERR_WRITE_FAILED, "Failed to end OTA update");
        }
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Validate image if configured
    if (g_ota_ctx->config.validate_image) {
        ESP_LOGI(TAG, "Validating OTA image...");
        
        esp_app_desc_t new_app_info;
        const esp_partition_t *partition = g_ota_ctx->update_partition;
        
        // Read app description from new firmware
        err = esp_ota_get_partition_description(partition, &new_app_info);
        if (err != ESP_OK) {
            set_error_state(OTA_ERR_VERIFY_FAILED, "Failed to get partition description");
            return err;
        }
        
        ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
        ESP_LOGI(TAG, "New firmware date: %s %s", new_app_info.date, new_app_info.time);
        
        // Update version info
        xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
        strncpy(g_ota_ctx->status.new_version, new_app_info.version, 
                sizeof(g_ota_ctx->status.new_version) - 1);
        xSemaphoreGive(g_ota_ctx->mutex);
    }
    
    update_state(OTA_STATE_APPLYING);
    
    // Set boot partition
    err = esp_ota_set_boot_partition(g_ota_ctx->update_partition);
    if (err != ESP_OK) {
        set_error_state(OTA_ERR_SET_BOOT_FAILED, "Failed to set boot partition");
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Update statistics
    g_ota_ctx->successful_updates++;
    
    // Update final status
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    g_ota_ctx->status.update_duration_ms = get_time_ms() - g_ota_ctx->start_time;
    g_ota_ctx->status.progress_percent = 100;
    xSemaphoreGive(g_ota_ctx->mutex);
    
    update_state(OTA_STATE_SUCCESS);
    
    ESP_LOGI(TAG, "OTA update completed successfully");
    ESP_LOGI(TAG, "Restart required to boot new firmware");
    
    return ESP_OK;
}

// Abort OTA update process
esp_err_t ota_manager_abort(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return ESP_OK;
    }
    
    if (g_ota_ctx->state == OTA_STATE_IDLE) {
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "Aborting OTA update");
    
    // Abort OTA if handle is valid
    if (g_ota_ctx->update_handle) {
        esp_ota_abort(g_ota_ctx->update_handle);
        g_ota_ctx->update_handle = 0;
    }
    
    // Update statistics
    if (g_ota_ctx->state != OTA_STATE_SUCCESS) {
        g_ota_ctx->failed_updates++;
    }
    
    // Reset state
    set_error_state(OTA_ERR_ABORTED, "OTA update aborted");
    
    // Clear update context
    g_ota_ctx->update_partition = NULL;
    g_ota_ctx->bytes_written = 0;
    g_ota_ctx->expected_size = 0;
    
    // Reset to idle
    update_state(OTA_STATE_IDLE);
    
    return ESP_OK;
}

// Get current OTA status
esp_err_t ota_manager_get_status(ota_status_t *status) {
    if (!g_ota_ctx || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    memcpy(status, &g_ota_ctx->status, sizeof(ota_status_t));
    
    // Update current firmware version
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(g_ota_ctx->running_partition, &running_app_info) == ESP_OK) {
        strncpy(status->firmware_version, running_app_info.version, 
                sizeof(status->firmware_version) - 1);
    }
    xSemaphoreGive(g_ota_ctx->mutex);
    
    return ESP_OK;
}

// Get current OTA state
ota_state_t ota_manager_get_state(void) {
    if (!g_ota_ctx) {
        return OTA_STATE_IDLE;
    }
    
    return g_ota_ctx->state;
}

// Check if OTA update is in progress
bool ota_manager_is_in_progress(void) {
    if (!g_ota_ctx) {
        return false;
    }
    
    return (g_ota_ctx->state != OTA_STATE_IDLE && 
            g_ota_ctx->state != OTA_STATE_SUCCESS && 
            g_ota_ctx->state != OTA_STATE_ERROR);
}

// Get progress percentage
uint8_t ota_manager_get_progress(void) {
    if (!g_ota_ctx || g_ota_ctx->expected_size == 0) {
        return 0;
    }
    
    return g_ota_ctx->status.progress_percent;
}

// Validate OTA partition
esp_err_t ota_manager_validate_partition(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(partition, &state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get partition state: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Partition state: %d", state);
    
    if (state == ESP_OTA_IMG_VALID) {
        ESP_LOGI(TAG, "Current partition is valid");
        return ESP_OK;
    } else if (state == ESP_OTA_IMG_UNDEFINED || state == ESP_OTA_IMG_NEW) {
        ESP_LOGW(TAG, "Current partition state is undefined or new");
        return ESP_ERR_INVALID_STATE;
    } else {
        ESP_LOGE(TAG, "Current partition is invalid or aborted");
        return ESP_FAIL;
    }
}

// Mark current firmware as valid
esp_err_t ota_manager_mark_valid(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Marking current firmware as valid");
    
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark app as valid: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Firmware marked as valid, rollback cancelled");
    return ESP_OK;
}

// Check if rollback is possible
bool ota_manager_can_rollback(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return false;
    }
    
    const esp_partition_t *partition = esp_ota_get_last_invalid_partition();
    return (partition != NULL);
}

// Perform firmware rollback
esp_err_t ota_manager_rollback(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGW(TAG, "Initiating firmware rollback");
    
    update_state(OTA_STATE_ROLLBACK);
    
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to rollback: %s", esp_err_to_name(err));
        set_error_state(OTA_ERR_SET_BOOT_FAILED, "Rollback failed");
        return err;
    }
    
    // This line should not be reached as system will reboot
    return ESP_OK;
}

// Get current firmware version info
esp_err_t ota_manager_get_firmware_info(ota_firmware_info_t *info) {
    if (!g_ota_ctx || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_app_desc_t app_desc;
    esp_err_t err = esp_ota_get_partition_description(partition, &app_desc);
    if (err != ESP_OK) {
        return err;
    }
    
    // Fill firmware info
    strncpy(info->version, app_desc.version, sizeof(info->version) - 1);
    info->version[sizeof(info->version) - 1] = '\0';
    
    // Calculate size (approximate)
    info->size = partition->size;
    
    // Build timestamp from date and time strings
    info->build_timestamp = 0;  // Would need parsing of app_desc.date and app_desc.time
    
    // Copy secure version info
    info->secure_signed = (app_desc.secure_version > 0);
    
    // Git hash if available
    memset(info->build_hash, 0, sizeof(info->build_hash));
    
    return ESP_OK;
}

// Get running partition info
const esp_partition_t *ota_manager_get_running_partition(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return NULL;
    }
    
    return g_ota_ctx->running_partition;
}

// Get next update partition
const esp_partition_t *ota_manager_get_update_partition(void) {
    if (!g_ota_ctx || !g_ota_ctx->initialized) {
        return NULL;
    }
    
    return esp_ota_get_next_update_partition(NULL);
}

// Calculate CRC32 of current firmware
esp_err_t ota_manager_calculate_crc32(uint32_t *crc32) {
    if (!g_ota_ctx || !crc32) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Read and calculate CRC32 in chunks
    const size_t chunk_size = 2048;
    uint8_t *buffer = malloc(chunk_size);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    
    uint32_t calc_crc = 0;
    size_t offset = 0;
    size_t remaining = partition->size;
    
    while (remaining > 0) {
        size_t to_read = (remaining < chunk_size) ? remaining : chunk_size;
        
        esp_err_t err = esp_partition_read(partition, offset, buffer, to_read);
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }
        
        calc_crc = esp_crc32_le(calc_crc, buffer, to_read);
        
        offset += to_read;
        remaining -= to_read;
    }
    
    free(buffer);
    *crc32 = calc_crc;
    
    return ESP_OK;
}

// Set progress callback
void ota_manager_set_progress_callback(void (*callback)(uint8_t, uint32_t, uint32_t)) {
    if (g_ota_ctx) {
        g_ota_ctx->progress_callback = callback;
    }
}

// Set state callback
void ota_manager_set_state_callback(void (*callback)(ota_state_t, ota_error_code_t)) {
    if (g_ota_ctx) {
        g_ota_ctx->state_callback = callback;
    }
}

// Get error message
const char *ota_manager_get_error_message(void) {
    if (!g_ota_ctx) {
        return "OTA manager not initialized";
    }
    
    return g_ota_ctx->status.error_message;
}

// Clear OTA status
void ota_manager_clear_status(void) {
    if (!g_ota_ctx) {
        return;
    }
    
    xSemaphoreTake(g_ota_ctx->mutex, portMAX_DELAY);
    
    g_ota_ctx->state = OTA_STATE_IDLE;
    g_ota_ctx->error_code = OTA_ERR_NONE;
    memset(&g_ota_ctx->status, 0, sizeof(ota_status_t));
    g_ota_ctx->status.state = OTA_STATE_IDLE;
    g_ota_ctx->status.error_code = OTA_ERR_NONE;
    g_ota_ctx->bytes_written = 0;
    g_ota_ctx->expected_size = 0;
    g_ota_ctx->update_handle = 0;
    g_ota_ctx->update_partition = NULL;
    
    xSemaphoreGive(g_ota_ctx->mutex);
    
    ESP_LOGI(TAG, "OTA status cleared");
}

// Get OTA statistics
esp_err_t ota_manager_get_statistics(uint32_t *successful_updates, 
                                     uint32_t *failed_updates,
                                     uint64_t *total_bytes) {
    if (!g_ota_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (successful_updates) {
        *successful_updates = g_ota_ctx->successful_updates;
    }
    
    if (failed_updates) {
        *failed_updates = g_ota_ctx->failed_updates;
    }
    
    if (total_bytes) {
        *total_bytes = g_ota_ctx->total_bytes_written;
    }
    
    return ESP_OK;
}