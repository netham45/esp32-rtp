#include "web_server.h"
#include "routes/routes.h"
#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "web_server";
static httpd_handle_t s_httpd_handle = NULL;


/**
 * Start the web server
 */
esp_err_t web_server_start(void)
{
    ESP_LOGI(TAG, "Starting web server");

    // If server is already running, stop it first
    if (s_httpd_handle != NULL) {
        ESP_LOGI(TAG, "Web server already running, stopping first");
        web_server_stop();
    }

    // Configure the HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 55;  // Increased to accommodate all handlers

    // Increase buffer size for requests
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    // Bind the server to ANY address instead of just the AP interface
    // This allows it to be accessible from both AP and STA interfaces
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.task_priority = 5;
    config.core_id = 0;

    // You might need to modify sdkconfig to increase HTTPD_MAX_REQ_HDR_LEN and HTTPD_MAX_URI_LEN

    // Start the HTTP server
    esp_err_t ret = httpd_start(&s_httpd_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: 0x%x", ret);
        return ret;
    }

    // Register all routes using the modular route system
    ret = register_all_routes(s_httpd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register routes: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Web server started successfully");
    return ESP_OK;
}

/**
 * Stop the web server
 */
esp_err_t web_server_stop(void)
{
    ESP_LOGI(TAG, "Stopping web server");
    
    // mDNS discovery is now managed by mdns_service, no need to stop it here

    if (s_httpd_handle == NULL) {
        ESP_LOGW(TAG, "Web server not running");
        return ESP_OK;
    }

    // Stop HTTP server
    esp_err_t ret = httpd_stop(s_httpd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: 0x%x", ret);
        return ret;
    }

    s_httpd_handle = NULL;
    return ESP_OK;
}

/**
 * Check if the web server is running
 */
bool web_server_is_running(void)
{
    return s_httpd_handle != NULL;
}
