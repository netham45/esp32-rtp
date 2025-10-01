#include "battery_routes.h"
#include "bq25895/bq25895_web.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "battery_routes";

/**
 * GET handler for BQ25895 HTML page
 */
static esp_err_t bq25895_html_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /bq25895");

    // Get the HTML content
    const char *html = bq25895_web_get_html();

    // Set content type
    httpd_resp_set_type(req, "text/html");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, html, strlen(html));

    return ret;
}

/**
 * GET handler for BQ25895 CSS
 */
static esp_err_t bq25895_css_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /bq25895/css");

    // Get the CSS content
    const char *css = bq25895_web_get_css();

    // Set content type
    httpd_resp_set_type(req, "text/css");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, css, strlen(css));

    return ret;
}

/**
 * GET handler for BQ25895 JavaScript
 */
static esp_err_t bq25895_js_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /bq25895/js");

    // Get the JavaScript content
    const char *js = bq25895_web_get_js();

    // Set content type
    httpd_resp_set_type(req, "application/javascript");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, js, strlen(js));

    return ret;
}

/**
 * Handler for BQ25895 API requests
 */
static esp_err_t bq25895_api_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[DEBUG] bq25895_api_handler - Request pointer: %p", req);
    
    // Check for NULL request pointer
    if (!req) {
        ESP_LOGE(TAG, "Request pointer is NULL in bq25895_api_handler");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Handling request for %s %s", req->method == HTTP_GET ? "GET" : "POST", req->uri);

    // Get content length
    size_t content_len = req->content_len;
    char *content = NULL;

    // Read content if it's a POST request
    if (req->method == HTTP_POST && content_len > 0) {
        content = malloc(content_len + 1);
        if (!content) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        int ret = httpd_req_recv(req, content, content_len);
        if (ret <= 0) {
            free(content);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        content[content_len] = '\0';
    }

    // Prepare response
    char *response = NULL;
    size_t response_len = 0;

    // Handle the request using the function from bq25895_web.c
    esp_err_t ret = bq25895_web_handle_request(req->uri,
                                              req->method == HTTP_GET ? "GET" : "POST",
                                              content,
                                              content_len,
                                              &response,
                                              &response_len);

    // Free content buffer
    if (content) {
        free(content);
    }

    if (ret != ESP_OK && response == NULL) { // Check if response is NULL in case of error
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to handle request");
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    ret = httpd_resp_send(req, response, response_len);

    // Free response buffer
    if (response) {
        free(response);
    }

    return ret;
}

/**
 * Register all battery management (BQ25895) routes with the HTTP server
 */
esp_err_t register_battery_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Register BQ25895 HTML page handler
    httpd_uri_t bq25895_html = {
        .uri       = "/bq25895",
        .method    = HTTP_GET,
        .handler   = bq25895_html_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_html);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /bq25895 handler: 0x%x", ret);
        return ret;
    }

    // Register BQ25895 CSS handler
    httpd_uri_t bq25895_css = {
        .uri       = "/bq25895/css",
        .method    = HTTP_GET,
        .handler   = bq25895_css_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_css);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /bq25895/css handler: 0x%x", ret);
        return ret;
    }

    // Register BQ25895 JavaScript handler
    httpd_uri_t bq25895_js = {
        .uri       = "/bq25895/js",
        .method    = HTTP_GET,
        .handler   = bq25895_js_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_js);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /bq25895/js handler: 0x%x", ret);
        return ret;
    }

    // Register BQ25895 API endpoints

    // GET /api/bq25895/status
    httpd_uri_t bq25895_status_get = {
        .uri       = "/api/bq25895/status",
        .method    = HTTP_GET,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_status_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/status GET handler: 0x%x", ret);
        return ret;
    }

    // POST /api/bq25895/status
    httpd_uri_t bq25895_status_post = {
        .uri       = "/api/bq25895/status",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_status_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/status POST handler: 0x%x", ret);
        return ret;
    }

    // GET /api/bq25895/config
    httpd_uri_t bq25895_config_get = {
        .uri       = "/api/bq25895/config",
        .method    = HTTP_GET,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_config_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/config GET handler: 0x%x", ret);
        return ret;
    }

    // POST /api/bq25895/config
    httpd_uri_t bq25895_config_post = {
        .uri       = "/api/bq25895/config",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_config_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/config POST handler: 0x%x", ret);
        return ret;
    }

    // POST /api/bq25895/reset
    httpd_uri_t bq25895_reset_post = {
        .uri       = "/api/bq25895/reset",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_reset_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/reset handler: 0x%x", ret);
        return ret;
    }

    // POST /api/bq25895/ce_pin
    httpd_uri_t bq25895_ce_pin_post = {
        .uri       = "/api/bq25895/ce_pin",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_ce_pin_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/ce_pin handler: 0x%x", ret);
        return ret;
    }

    // GET /api/bq25895/register
    httpd_uri_t bq25895_register_get = {
        .uri       = "/api/bq25895/register",
        .method    = HTTP_GET,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_register_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/register GET handler: 0x%x", ret);
        return ret;
    }

    // POST /api/bq25895/register
    httpd_uri_t bq25895_register_post = {
        .uri       = "/api/bq25895/register",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_register_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/bq25895/register POST handler: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Battery routes registered successfully");
    return ESP_OK;
}