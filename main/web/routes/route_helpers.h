/**
 * @file route_helpers.h
 * @brief Shared utility functions for web server route handlers
 * 
 * This module provides reusable helper functions that are used across
 * multiple route handlers in the web server.
 */

#ifndef ROUTE_HELPERS_H
#define ROUTE_HELPERS_H

#include "esp_http_server.h"
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send HTML content in chunks with placeholder replacement
 * 
 * This function sends HTML in chunks to avoid large memory allocations.
 * It also replaces template placeholders with actual values:
 * - {{DEVICE_NAME}} - Device name
 * - {{CURRENT_SSID}} - Currently connected WiFi SSID
 * - {{CONNECTION_STATUS}} - WiFi connection status with IP

 * @param req HTTP request structure
 * @param html_start Pointer to the start of HTML content
 * @param html_size Size of HTML content in bytes
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t send_html_chunked(httpd_req_t *req, const char* html_start, size_t html_size);

/**
 * @brief Send static content without modification using chunked transfer
 *
 * This helper streams large embedded assets (CSS/JS/HTML) directly from flash
 * in manageable pieces. Using chunked transfer avoids the large temporary
 * allocations performed by httpd_resp_send() when asked to write big payloads
 * in a single call.
 *
 * @param req HTTP request structure
 * @param data_start Pointer to the start of the embedded data
 * @param data_size Size of the embedded data in bytes
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t send_static_chunked(httpd_req_t *req, const uint8_t *data_start, size_t data_size);

/**
 * @brief URL decode a string (application/x-www-form-urlencoded)
 * 
 * Decodes URL-encoded strings by:
 * - Converting '+' to space
 * - Converting '%XX' hex sequences to actual characters
 * 
 * @param src Source URL-encoded string
 * @param dst Destination buffer for decoded string
 * @param dst_size Size of destination buffer
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if buffer too small
 * 
 * @note The destination buffer should be at least as large as the source
 */
esp_err_t url_decode(const char *src, char *dst, size_t dst_size);

#ifdef __cplusplus
}
#endif

#endif /* ROUTE_HELPERS_H */