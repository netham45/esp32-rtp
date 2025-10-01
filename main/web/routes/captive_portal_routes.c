#include "captive_portal_routes.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "captive_portal_routes";

// Simple HTML for redirecting to captive portal
static const char html_redirect[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta http-equiv=\"refresh\" content=\"0;URL='/'\">\n"
"</head>\n"
"<body>\n"
"    <p>Redirecting to captive portal...</p>\n"
"</body>\n"
"</html>\n";

// HTML for Apple Captive Network Assistant detection
static const char html_apple_cna[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <title>Success</title>\n"
"</head>\n"
"<body>\n"
"    <h1>Success</h1>\n"
"</body>\n"
"</html>\n";

/**
 * GET handler for Apple Captive Network Assistant detection
 */
static esp_err_t apple_cna_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for Apple CNA");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_apple_cna, strlen(html_apple_cna));

    return ESP_OK;
}

/**
 * GET handler for all other URIs (redirect to captive portal)
 * This is the catch-all handler for captive portal functionality
 */
static esp_err_t redirect_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for URI: %s", req->uri);

    // Special case for Apple captive portal detection (handled by a separate handler)
    if (strcmp(req->uri, "/hotspot-detect.html") == 0) {
        return apple_cna_get_handler(req);
    }

    // Handle favicon.ico requests silently (browsers often request this)
    if (strcmp(req->uri, "/favicon.ico") == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }

    // For all other URIs, redirect to the captive portal
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_redirect, strlen(html_redirect));

    return ESP_OK;
}

/**
 * Register all captive portal related routes
 */
esp_err_t register_captive_portal_routes(httpd_handle_t server)
{
    esp_err_t ret;

    // Handle Apple captive portal detection
    httpd_uri_t apple_cna = {
        .uri       = "/hotspot-detect.html",
        .method    = HTTP_GET,
        .handler   = apple_cna_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &apple_cna);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /hotspot-detect.html handler: 0x%x", ret);
        return ret;
    }

    // Register catch-all handler (must be registered last)
    httpd_uri_t redirect = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = redirect_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &redirect);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /* handler: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Captive portal routes registered successfully");
    return ESP_OK;
}