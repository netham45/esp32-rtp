#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// OTA states
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_APPLYING,
    OTA_STATE_SUCCESS,
    OTA_STATE_ERROR,
    OTA_STATE_ROLLBACK
} ota_state_t;

// OTA error codes
typedef enum {
    OTA_ERR_NONE = 0,
    OTA_ERR_INVALID_SIZE,
    OTA_ERR_INVALID_MAGIC,
    OTA_ERR_INVALID_VERSION,
    OTA_ERR_PARTITION_NOT_FOUND,
    OTA_ERR_WRITE_FAILED,
    OTA_ERR_VERIFY_FAILED,
    OTA_ERR_SET_BOOT_FAILED,
    OTA_ERR_MEMORY_ALLOC,
    OTA_ERR_NETWORK,
    OTA_ERR_TIMEOUT,
    OTA_ERR_ABORTED
} ota_error_code_t;

// OTA update status structure
typedef struct {
    ota_state_t state;
    ota_error_code_t error_code;
    uint32_t total_size;
    uint32_t received_size;
    uint8_t progress_percent;
    uint32_t update_start_time;
    uint32_t update_duration_ms;
    char error_message[128];
    char firmware_version[32];
    char new_version[32];
} ota_status_t;

// OTA configuration structure
typedef struct {
    size_t buffer_size;              // Buffer size for OTA writes (default: 4096)
    uint32_t timeout_ms;              // Timeout for OTA operations
    bool enable_rollback_protection;  // Enable rollback protection
    bool validate_image;              // Validate image before applying
    uint8_t max_retry_count;          // Maximum retry count for failed operations
    uint32_t watchdog_timeout_ms;     // Watchdog timeout during OTA
} ota_config_t;

// OTA firmware info structure
typedef struct {
    char version[32];
    uint32_t size;
    uint32_t crc32;
    uint32_t build_timestamp;
    char build_hash[41];  // Git commit hash
    bool secure_signed;
} ota_firmware_info_t;

// Function prototypes

/**
 * @brief Initialize OTA manager
 * 
 * @param config OTA configuration (NULL for defaults)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_manager_init(const ota_config_t *config);

/**
 * @brief Deinitialize OTA manager and free resources
 * 
 * @return ESP_OK on success
 */
esp_err_t ota_manager_deinit(void);

/**
 * @brief Start OTA update process
 * 
 * @param expected_size Expected firmware size (0 if unknown)
 * @param firmware_info Optional firmware info for validation
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_manager_start(uint32_t expected_size, const ota_firmware_info_t *firmware_info);

/**
 * @brief Write data chunk to OTA partition
 * 
 * @param data Data buffer to write
 * @param size Size of data to write
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_manager_write(const uint8_t *data, size_t size);

/**
 * @brief Complete OTA update and validate image
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_manager_complete(void);

/**
 * @brief Abort OTA update process
 * 
 * @return ESP_OK on success
 */
esp_err_t ota_manager_abort(void);

/**
 * @brief Get current OTA status
 * 
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t ota_manager_get_status(ota_status_t *status);

/**
 * @brief Get current OTA state
 * 
 * @return Current OTA state
 */
ota_state_t ota_manager_get_state(void);

/**
 * @brief Check if OTA update is in progress
 * 
 * @return true if update is in progress, false otherwise
 */
bool ota_manager_is_in_progress(void);

/**
 * @brief Get progress percentage
 * 
 * @return Progress percentage (0-100)
 */
uint8_t ota_manager_get_progress(void);

/**
 * @brief Validate OTA partition
 * 
 * @return ESP_OK if partition is valid
 */
esp_err_t ota_manager_validate_partition(void);

/**
 * @brief Mark current firmware as valid (for rollback protection)
 * 
 * @return ESP_OK on success
 */
esp_err_t ota_manager_mark_valid(void);

/**
 * @brief Check if rollback is possible
 * 
 * @return true if rollback is possible
 */
bool ota_manager_can_rollback(void);

/**
 * @brief Perform firmware rollback to previous version
 * 
 * @return ESP_OK on success
 */
esp_err_t ota_manager_rollback(void);

/**
 * @brief Get current firmware version info
 * 
 * @param info Pointer to firmware info structure to fill
 * @return ESP_OK on success
 */
esp_err_t ota_manager_get_firmware_info(ota_firmware_info_t *info);

/**
 * @brief Get running partition info
 * 
 * @return Pointer to partition info or NULL on error
 */
const esp_partition_t *ota_manager_get_running_partition(void);

/**
 * @brief Get next update partition
 * 
 * @return Pointer to partition info or NULL on error
 */
const esp_partition_t *ota_manager_get_update_partition(void);

/**
 * @brief Calculate CRC32 of current firmware
 * 
 * @param crc32 Pointer to store calculated CRC32
 * @return ESP_OK on success
 */
esp_err_t ota_manager_calculate_crc32(uint32_t *crc32);

/**
 * @brief Set callback for OTA progress updates
 * 
 * @param callback Progress callback function (percent, received, total)
 */
void ota_manager_set_progress_callback(void (*callback)(uint8_t, uint32_t, uint32_t));

/**
 * @brief Set callback for OTA state changes
 * 
 * @param callback State change callback function
 */
void ota_manager_set_state_callback(void (*callback)(ota_state_t, ota_error_code_t));

/**
 * @brief Get error message for last error
 * 
 * @return Error message string
 */
const char *ota_manager_get_error_message(void);

/**
 * @brief Clear OTA status and reset to idle
 */
void ota_manager_clear_status(void);

/**
 * @brief Get OTA statistics
 * 
 * @param successful_updates Pointer to store successful update count
 * @param failed_updates Pointer to store failed update count
 * @param total_bytes Pointer to store total bytes written
 * @return ESP_OK on success
 */
esp_err_t ota_manager_get_statistics(uint32_t *successful_updates, 
                                     uint32_t *failed_updates,
                                     uint64_t *total_bytes);

#ifdef __cplusplus
}
#endif

#endif // OTA_MANAGER_H