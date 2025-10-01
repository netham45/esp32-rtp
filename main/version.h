#ifndef VERSION_H
#define VERSION_H

// Firmware version information
#define FIRMWARE_VERSION_MAJOR  1
#define FIRMWARE_VERSION_MINOR  0
#define FIRMWARE_VERSION_PATCH  1

// Build number (can be auto-incremented by build system)
#define FIRMWARE_BUILD_NUMBER   3

// Version string format: "major.minor.patch-build"
#define FIRMWARE_VERSION_STRING "1.0.1-3"

// Full version string with additional info
#define FIRMWARE_VERSION_FULL   "ESP32 Scream Receiver v1.0.1-3"

// Application name
#define FIRMWARE_APP_NAME       "esp32-scream-receiver"

// Manufacturer/Company name
#define FIRMWARE_MANUFACTURER   "ESP32 Scream"

// Product name
#define FIRMWARE_PRODUCT_NAME   "Scream Receiver"

// Build configuration
#ifdef CONFIG_IDF_TARGET_ESP32
    #define FIRMWARE_PLATFORM   "ESP32"
#elif CONFIG_IDF_TARGET_ESP32S3
    #define FIRMWARE_PLATFORM   "ESP32-S3"
#else
    #define FIRMWARE_PLATFORM   "Unknown"
#endif

// Build date and time (populated by build system)
#ifndef FIRMWARE_BUILD_DATE
    #define FIRMWARE_BUILD_DATE __DATE__
#endif

#ifndef FIRMWARE_BUILD_TIME
    #define FIRMWARE_BUILD_TIME __TIME__
#endif

// Git commit hash (can be populated by build system)
#ifndef FIRMWARE_GIT_COMMIT
    #define FIRMWARE_GIT_COMMIT "unknown"
#endif

// Git branch (can be populated by build system)
#ifndef FIRMWARE_GIT_BRANCH
    #define FIRMWARE_GIT_BRANCH "unknown"
#endif

// Build type
#ifdef CONFIG_OPTIMIZATION_LEVEL_DEBUG
    #define FIRMWARE_BUILD_TYPE "Debug"
#else
    #define FIRMWARE_BUILD_TYPE "Release"
#endif

// OTA URL endpoints (if using HTTP OTA)
#define OTA_UPDATE_URL          "https://update.example.com/firmware/"
#define OTA_CHECK_VERSION_URL   "https://update.example.com/version.json"
#define OTA_DOWNLOAD_URL        "https://update.example.com/download/"

// Minimum required version for OTA updates
#define OTA_MIN_VERSION_MAJOR   1
#define OTA_MIN_VERSION_MINOR   0
#define OTA_MIN_VERSION_PATCH   0
      
// Maximum firmware size must match OTA partition size from partitions.csv
// Each OTA partition is 1856KB (ota_0 and ota_1)
#define OTA_MAX_FIRMWARE_SIZE   (1856 * 1024)  // 1.856MB (1,900,544 bytes)

// OTA partition label
#define OTA_PARTITION_LABEL     "ota"

// Version comparison macros
#define VERSION_COMPARE(maj1, min1, pat1, maj2, min2, pat2) \
    ((maj1) != (maj2) ? ((maj1) > (maj2) ? 1 : -1) : \
     (min1) != (min2) ? ((min1) > (min2) ? 1 : -1) : \
     (pat1) != (pat2) ? ((pat1) > (pat2) ? 1 : -1) : 0)

#define VERSION_IS_NEWER(maj, min, pat) \
    (VERSION_COMPARE(maj, min, pat, \
                     FIRMWARE_VERSION_MAJOR, \
                     FIRMWARE_VERSION_MINOR, \
                     FIRMWARE_VERSION_PATCH) > 0)

#define VERSION_IS_OLDER(maj, min, pat) \
    (VERSION_COMPARE(maj, min, pat, \
                     FIRMWARE_VERSION_MAJOR, \
                     FIRMWARE_VERSION_MINOR, \
                     FIRMWARE_VERSION_PATCH) < 0)

#define VERSION_IS_EQUAL(maj, min, pat) \
    (VERSION_COMPARE(maj, min, pat, \
                     FIRMWARE_VERSION_MAJOR, \
                     FIRMWARE_VERSION_MINOR, \
                     FIRMWARE_VERSION_PATCH) == 0)

// Helper function prototypes for version handling
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get firmware version as a single integer
 * Format: (major << 16) | (minor << 8) | patch
 * 
 * @return Version as integer
 */
static inline uint32_t version_get_integer(void) {
    return (FIRMWARE_VERSION_MAJOR << 16) | 
           (FIRMWARE_VERSION_MINOR << 8) | 
           FIRMWARE_VERSION_PATCH;
}

/**
 * @brief Parse version string to components
 * 
 * @param version_str Version string (e.g., "1.2.3")
 * @param major Pointer to store major version
 * @param minor Pointer to store minor version
 * @param patch Pointer to store patch version
 * @return true if parsing successful
 */
static inline bool version_parse_string(const char *version_str, 
                                        uint8_t *major, 
                                        uint8_t *minor, 
                                        uint8_t *patch) {
    if (!version_str || !major || !minor || !patch) {
        return false;
    }
    
    int maj, min, pat;
    if (sscanf(version_str, "%d.%d.%d", &maj, &min, &pat) == 3) {
        *major = (uint8_t)maj;
        *minor = (uint8_t)min;
        *patch = (uint8_t)pat;
        return true;
    }
    
    return false;
}

/**
 * @brief Format version components to string
 * 
 * @param buffer Buffer to store version string
 * @param size Buffer size
 * @param major Major version
 * @param minor Minor version
 * @param patch Patch version
 * @return Number of characters written
 */
static inline int version_format_string(char *buffer, 
                                        size_t size,
                                        uint8_t major, 
                                        uint8_t minor, 
                                        uint8_t patch) {
    if (!buffer || size == 0) {
        return 0;
    }
    
    return snprintf(buffer, size, "%u.%u.%u", major, minor, patch);
}

/**
 * @brief Get full version information string
 * 
 * @param buffer Buffer to store version info
 * @param size Buffer size
 * @return Number of characters written
 */
static inline int version_get_full_info(char *buffer, size_t size) {
    if (!buffer || size == 0) {
        return 0;
    }
    
    return snprintf(buffer, size, 
                   "%s v%s (Build: %d, Platform: %s, Date: %s %s)",
                   FIRMWARE_APP_NAME,
                   FIRMWARE_VERSION_STRING,
                   FIRMWARE_BUILD_NUMBER,
                   FIRMWARE_PLATFORM,
                   FIRMWARE_BUILD_DATE,
                   FIRMWARE_BUILD_TIME);
}

#ifdef __cplusplus
}
#endif

#endif // VERSION_H