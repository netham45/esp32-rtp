#include "static_routes.h"
#include "route_helpers.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "static_routes";

// External declarations for embedded web files
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t script_js_start[] asm("_binary_script_js_start");
extern const uint8_t script_js_end[] asm("_binary_script_js_end");
// Wizard files
extern const uint8_t wizard_html_start[] asm("_binary_wizard_html_start");
extern const uint8_t wizard_html_end[] asm("_binary_wizard_html_end");
extern const uint8_t wizard_css_start[] asm("_binary_wizard_css_start");
extern const uint8_t wizard_css_end[] asm("_binary_wizard_css_end");
extern const uint8_t wizard_js_start[] asm("_binary_wizard_js_start");
extern const uint8_t wizard_js_end[] asm("_binary_wizard_js_end");
// SAP files
extern const uint8_t sap_html_start[] asm("_binary_sap_html_start");
extern const uint8_t sap_html_end[] asm("_binary_sap_html_end");
extern const uint8_t sap_js_start[] asm("_binary_sap_js_start");
extern const uint8_t sap_js_end[] asm("_binary_sap_js_end");
// BQ25895 files
extern const uint8_t bq25895_html_start[] asm("_binary_bq25895_html_start");
extern const uint8_t bq25895_html_end[] asm("_binary_bq25895_html_end");
extern const uint8_t bq25895_css_start[] asm("_binary_bq25895_css_start");
extern const uint8_t bq25895_css_end[] asm("_binary_bq25895_css_end");
extern const uint8_t bq25895_js_start[] asm("_binary_bq25895_js_start");
extern const uint8_t bq25895_js_end[] asm("_binary_bq25895_js_end");

/**
 * GET handler for the root page (index.html)
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /");

    // Get the size of the embedded index.html file
    size_t index_html_size = index_html_end - index_html_start;
    
    // Use chunked sending to avoid large memory allocations
    return send_html_chunked(req, (const char*)index_html_start, index_html_size);
}

/**
 * GET handler for the CSS file (styles.css)
 */
static esp_err_t css_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /styles.css");

    // Set content type
    httpd_resp_set_type(req, "text/css");

    // Send the CSS file
    size_t css_size = styles_css_end - styles_css_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)styles_css_start, css_size);

    return ret;
}

/**
 * GET handler for the JavaScript file (script.js)
 */
static esp_err_t js_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /script.js");

    // Set content type
    httpd_resp_set_type(req, "application/javascript");

    // Send the JavaScript file
    size_t js_size = script_js_end - script_js_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)script_js_start, js_size);

    return ret;
}

/**
 * GET handler for the wizard HTML file (wizard.html)
 */
static esp_err_t wizard_html_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /wizard.html");

    // Get the size of the embedded wizard.html file
    size_t wizard_html_size = wizard_html_end - wizard_html_start;
    
    // Use chunked sending to avoid large memory allocations
    return send_html_chunked(req, (const char*)wizard_html_start, wizard_html_size);
}

/**
 * GET handler for the wizard CSS file (wizard.css)
 */
static esp_err_t wizard_css_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /wizard.css");

    // Set content type
    httpd_resp_set_type(req, "text/css");

    // Send the CSS file
    size_t css_size = wizard_css_end - wizard_css_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)wizard_css_start, css_size);

    return ret;
}

/**
 * GET handler for the wizard JavaScript file (wizard.js)
 */
static esp_err_t wizard_js_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /wizard.js");

    // Set content type
    httpd_resp_set_type(req, "application/javascript");

    // Send the JavaScript file
    size_t js_size = wizard_js_end - wizard_js_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)wizard_js_start, js_size);

    return ret;
}

/**
 * GET handler for SAP HTML page
 */
static esp_err_t sap_html_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /sap.html");

    // Set content type
    httpd_resp_set_type(req, "text/html");

    // Send the HTML file
    size_t html_size = sap_html_end - sap_html_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)sap_html_start, html_size);

    return ret;
}

/**
 * GET handler for SAP JavaScript file
 */
static esp_err_t sap_js_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /sap.js");

    // Set content type
    httpd_resp_set_type(req, "application/javascript");

    // Send the JavaScript file
    size_t js_size = sap_js_end - sap_js_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)sap_js_start, js_size);

    return ret;
}

/**
 * GET handler for BQ25895 HTML page
 */
static esp_err_t bq25895_html_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /bq25895");
    size_t html_size = bq25895_html_end - bq25895_html_start;
    return send_html_chunked(req, (const char *)bq25895_html_start, html_size);
}

/**
 * GET handler for BQ25895 CSS
 */
static esp_err_t bq25895_css_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /bq25895/css");
    httpd_resp_set_type(req, "text/css");
    size_t css_size = bq25895_css_end - bq25895_css_start;
    return httpd_resp_send(req, (const char *)bq25895_css_start, css_size);
}

/**
 * GET handler for BQ25895 JavaScript
 */
static esp_err_t bq25895_js_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /bq25895/js");
    httpd_resp_set_type(req, "application/javascript");
    size_t js_size = bq25895_js_end - bq25895_js_start;
    return httpd_resp_send(req, (const char *)bq25895_js_start, js_size);
}

/**
 * Register all static content routes
 */
esp_err_t register_static_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Register root page
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &root);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register / handler");
        return ret;
    }

    // Register CSS file
    httpd_uri_t css = {
        .uri       = "/styles.css",
        .method    = HTTP_GET,
        .handler   = css_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &css);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /styles.css handler");
        return ret;
    }

    // Register JavaScript file
    httpd_uri_t js = {
        .uri       = "/script.js",
        .method    = HTTP_GET,
        .handler   = js_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &js);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /script.js handler");
        return ret;
    }

    // Register wizard HTML
    httpd_uri_t wizard_html = {
        .uri       = "/wizard.html",
        .method    = HTTP_GET,
        .handler   = wizard_html_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &wizard_html);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /wizard.html handler");
        return ret;
    }

    // Register wizard CSS
    httpd_uri_t wizard_css = {
        .uri       = "/wizard.css",
        .method    = HTTP_GET,
        .handler   = wizard_css_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &wizard_css);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /wizard.css handler");
        return ret;
    }

    // Register wizard JavaScript
    httpd_uri_t wizard_js_uri = {
        .uri       = "/wizard.js",
        .method    = HTTP_GET,
        .handler   = wizard_js_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &wizard_js_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /wizard.js handler");
        return ret;
    }

    // Register BQ25895 HTML
    httpd_uri_t bq25895_html = {
        .uri = "/bq25895",
        .method = HTTP_GET,
        .handler = bq25895_html_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_html);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /bq25895 handler");
        return ret;
    }

    // Register BQ25895 CSS
    httpd_uri_t bq25895_css = {
        .uri = "/bq25895/css",
        .method = HTTP_GET,
        .handler = bq25895_css_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_css);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /bq25895/css handler");
        return ret;
    }

    // Register BQ25895 JavaScript
    httpd_uri_t bq25895_js = {
        .uri = "/bq25895/js",
        .method = HTTP_GET,
        .handler = bq25895_js_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &bq25895_js);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /bq25895/js handler");
        return ret;
    }

    // Register SAP HTML
    httpd_uri_t sap_html = {
        .uri = "/sap.html",
        .method = HTTP_GET,
        .handler = sap_html_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &sap_html);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /sap.html handler");
        return ret;
    }

    // Register SAP JavaScript
    httpd_uri_t sap_js = {
        .uri = "/sap.js",
        .method = HTTP_GET,
        .handler = sap_js_handler,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &sap_js);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /sap.js handler");
        return ret;
    }

    ESP_LOGI(TAG, "All static routes registered successfully");
    return ESP_OK;
}