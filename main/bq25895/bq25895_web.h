/**
 * @file bq25895_web.h
 * @brief Web interface for BQ25895 battery charger IC
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BQ25895 web interface
 * 
 * @return ESP_OK on success
 */
esp_err_t bq25895_web_init(void);

/**
 * @brief Get the HTML content for the BQ25895 web interface
 * 
 * @return HTML content string
 */
const char *bq25895_web_get_html(void);

/**
 * @brief Get the JavaScript content for the BQ25895 web interface
 * 
 * @return JavaScript content string
 */
const char *bq25895_web_get_js(void);

/**
 * @brief Get the CSS content for the BQ25895 web interface
 * 
 * @return CSS content string
 */
const char *bq25895_web_get_css(void);

/**
 * @brief Handle HTTP requests for the BQ25895 web interface
 * 
 * @param uri URI of the request
 * @param method HTTP method
 * @param content Request content
 * @param content_len Length of request content
 * @param response Pointer to store response
 * @param response_len Pointer to store response length
 * @return ESP_OK on success
 */
esp_err_t bq25895_web_handle_request(const char *uri, const char *method, const char *content, size_t content_len, char **response, size_t *response_len);

#ifdef __cplusplus
}
#endif
