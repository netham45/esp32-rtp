#include "web_server.h"
#include "wifi/wifi_manager.h"
#include "config.h"
#include "bq25895/bq25895_web.h"
#include "bq25895/bq25895.h"
#include "lifecycle_manager.h"
#include "mdns/mdns_discovery.h"
#include "ota/ota_manager.h"
#include "version.h"
#include "logging/log_buffer.h"

// Removed external resume_playback() - now handled by lifecycle_manager

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

// Simple HTML for redirecting to captive portal
const char html_redirect[] =
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
const char html_apple_cna[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <title>Success</title>\n"
"</head>\n"
"<body>\n"
"    <h1>Success</h1>\n"
"</body>\n"
"</html>\n";
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "lwip/dns.h"
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#ifdef IS_USB
#include "usb/uac_host.h"
#endif

// DNS server for captive portal
#define DNS_PORT 53
// static esp_netif_t *s_dns_netif = NULL;  // Unused variable - commented out
static int s_dns_sock = -1;
static httpd_handle_t s_httpd_handle = NULL;

// Maximum number of networks to return in scan
#define MAX_SCAN_RESULTS 20

// Forward declarations
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t scan_get_handler(httpd_req_t *req);
static esp_err_t status_get_handler(httpd_req_t *req);
static esp_err_t settings_get_handler(httpd_req_t *req);
static esp_err_t settings_post_handler(httpd_req_t *req);
static esp_err_t connect_post_handler(httpd_req_t *req);
static esp_err_t reset_post_handler(httpd_req_t *req);
static esp_err_t start_pairing_handler(httpd_req_t *req);
static esp_err_t cancel_pairing_handler(httpd_req_t *req);
static esp_err_t redirect_get_handler(httpd_req_t *req);
static esp_err_t apple_cna_get_handler(httpd_req_t *req);
static esp_err_t bq25895_html_handler(httpd_req_t *req);
static esp_err_t bq25895_css_handler(httpd_req_t *req);
static esp_err_t bq25895_js_handler(httpd_req_t *req);
static esp_err_t bq25895_api_handler(httpd_req_t *req);
static void dns_server_task(void *pvParameters);
static esp_err_t discover_devices_handler(httpd_req_t *req);
static esp_err_t scream_devices_handler(httpd_req_t *req);
static esp_err_t ota_upload_handler(httpd_req_t *req);
static esp_err_t ota_status_handler(httpd_req_t *req);
static esp_err_t ota_version_handler(httpd_req_t *req);
static esp_err_t ota_rollback_handler(httpd_req_t *req);
static esp_err_t logs_get_handler(httpd_req_t *req);

/**
 * Start the DNS server for captive portal functionality
 */
static void start_dns_server(void)
{
    ESP_LOGI(TAG, "Starting DNS server for captive portal");

    struct sockaddr_in server_addr;

    // Create a UDP socket
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: %d", errno);
        return;
    }

    // Bind the socket to port 53 (DNS)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket: %d", errno);
        close(s_dns_sock);
        s_dns_sock = -1;
        return;
    }

    // Start DNS server task
    xTaskCreatePinnedToCore(dns_server_task, "dns_server", 4096, NULL, 1, NULL, 0);
}

/**
 * Stop the DNS server
 */
static void stop_dns_server(void)
{
    ESP_LOGI(TAG, "Stopping DNS server");

    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}

/**
 * Simple DNS server implementation for captive portal
 * It responds to all DNS queries with the AP's IP address (192.168.4.1)
 */
static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    ESP_LOGI(TAG, "DNS server task started");

    while (s_dns_sock >= 0) {
        // Wait for DNS query
        int len = recvfrom(s_dns_sock, rx_buffer, sizeof(rx_buffer), 0,
                         (struct sockaddr *)&client_addr, &client_addr_len);

        if (len < 0) {
            ESP_LOGE(TAG, "DNS recv error: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (len < 12) {
            // DNS header is at least 12 bytes
            continue;
        }

        // Log DNS request details (for debugging)
        ESP_LOGD(TAG, "Received DNS query of length %d", len);

        // Prepare DNS response
        // Copy the request header and modify it for response
        memcpy(tx_buffer, rx_buffer, len);

        // Set the response bit (QR)
        tx_buffer[2] |= 0x80;

        // Set authoritative answer bit (AA)
        tx_buffer[2] |= 0x04;

        // Clear the recursion available bit (RA)
        tx_buffer[3] |= 0x80;

        // Set the response code to 0 (no error) - clear the lower 4 bits of byte 3
        tx_buffer[3] &= 0xF0;

        // Get the number of queries
        uint16_t query_count = (rx_buffer[4] << 8) | rx_buffer[5];

        // Set answer count equal to query count
        tx_buffer[6] = rx_buffer[4];
        tx_buffer[7] = rx_buffer[5];

        // Set authority count and additional count to 0
        tx_buffer[8] = 0;
        tx_buffer[9] = 0;
        tx_buffer[10] = 0;
        tx_buffer[11] = 0;

        // Track position in the response buffer
        int response_len = len;

        // Find end of each query to add answers
        int query_pos = 12; // Start position of the queries section

        // Process all queries
        for (int i = 0; i < query_count && query_pos < len; i++) {
            // Find the end of the domain name
            // int question_start = query_pos;  // Unused variable - commented out

            // Print domain being queried for debugging
            char domain_name[256] = {0};
            int domain_pos = 0;
            int current_pos = query_pos;

            while (current_pos < len && rx_buffer[current_pos] != 0) {
                uint8_t label_len = rx_buffer[current_pos++];
                for (int j = 0; j < label_len && current_pos < len; j++) {
                    domain_name[domain_pos++] = rx_buffer[current_pos++];
                }
                if (rx_buffer[current_pos] != 0) {
                    domain_name[domain_pos++] = '.';
                }
            }
            domain_name[domain_pos] = '\0';
            ESP_LOGI(TAG, "DNS Query for domain: %s", domain_name);

            // Skip the domain name
            while (query_pos < len && rx_buffer[query_pos] != 0) {
                query_pos += rx_buffer[query_pos] + 1;
            }
            query_pos++; // Skip the terminating zero length

            // Skip QTYPE and QCLASS (4 bytes)
            uint16_t qtype = (rx_buffer[query_pos] << 8) | rx_buffer[query_pos + 1];
            query_pos += 4;

            // Only respond to A record queries
            if (qtype == 1) { // 1 = A record
                // Add answer section
                // NAME: pointer to the domain name in the question
                tx_buffer[response_len++] = 0xC0; // Compression pointer
                tx_buffer[response_len++] = 0x0C; // Pointer to position 12 (start of queries)

                // TYPE: A record (0x0001)
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x01;

                // CLASS: IN (0x0001)
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x01;

                // TTL: 300 seconds (5 minutes)
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x01;
                tx_buffer[response_len++] = 0x2C;

                // RDLENGTH: 4 bytes for IPv4 address
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x04;

                // RDATA: IP address (192.168.4.1)
                tx_buffer[response_len++] = 192;
                tx_buffer[response_len++] = 168;
                tx_buffer[response_len++] = 4;
                tx_buffer[response_len++] = 1;
            }
        }

        // Send the response
        int sent = sendto(s_dns_sock, tx_buffer, response_len, 0,
                        (struct sockaddr *)&client_addr, client_addr_len);

        if (sent < 0) {
            ESP_LOGE(TAG, "DNS send error: %d", errno);
        } else {
            ESP_LOGD(TAG, "Sent DNS response, length: %d", sent);
        }
    }

    ESP_LOGI(TAG, "DNS server task ended");
    vTaskDelete(NULL);
}

/**
 * Chunked sending of HTML with placeholder replacement
 * This function sends HTML in chunks to avoid large memory allocations
 */
static esp_err_t send_html_chunked(httpd_req_t *req, const char* html_start, size_t html_size)
{
    // Define chunk size - small enough to fit in memory easily
    #define CHUNK_SIZE 2048
    
    ESP_LOGI(TAG, "Sending HTML chunked, total size: %zu bytes", html_size);
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    
    // Set content type
    httpd_resp_set_type(req, "text/html");
    
    // Allocate a working buffer for chunks
    char *chunk_buffer = malloc(CHUNK_SIZE + 512); // Extra space for replacements
    if (!chunk_buffer) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        return httpd_resp_send_500(req);
    }
    
    // Get values for replacement once
    char device_name[] = "ESP32 RTP";
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = "Not configured";
    wifi_manager_get_current_ssid(ssid, sizeof(ssid));
    
    // Get connection status
    const char *status;
    char status_with_ip[64] = {0};
    wifi_manager_state_t state = wifi_manager_get_state();
    switch (state) {
        case WIFI_MANAGER_STATE_CONNECTED:
            {
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    snprintf(status_with_ip, sizeof(status_with_ip),
                            "Connected (IP: %s)", ip_str);
                    status = status_with_ip;
                } else {
                    status = "Connected";
                }
            }
            break;
        case WIFI_MANAGER_STATE_CONNECTING:
            status = "Connecting...";
            break;
        case WIFI_MANAGER_STATE_CONNECTION_FAILED:
            status = "Connection failed";
            break;
        case WIFI_MANAGER_STATE_AP_MODE:
            status = "Access Point Mode (192.168.4.1)";
            break;
        default:
            status = "Unknown";
            break;
    }
    
    // Process and send HTML in chunks
    size_t offset = 0;
    esp_err_t ret = ESP_OK;
    
    while (offset < html_size && ret == ESP_OK) {
        // Calculate chunk size for this iteration
        size_t chunk_size = (html_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (html_size - offset);
        
        // Copy chunk to buffer
        memcpy(chunk_buffer, html_start + offset, chunk_size);
        chunk_buffer[chunk_size] = '\0';
        
        // Simple inline replacement for placeholders in this chunk
        // Note: This is simplified - placeholders that span chunk boundaries won't be replaced
        char *pos;
        
        // Replace {{DEVICE_NAME}}
        if ((pos = strstr(chunk_buffer, "{{DEVICE_NAME}}")) != NULL) {
            size_t before_len = pos - chunk_buffer;
            size_t after_start = before_len + strlen("{{DEVICE_NAME}}");
            size_t after_len = chunk_size - after_start;
            
            // Create new buffer with replacement
            memmove(pos + strlen(device_name), pos + strlen("{{DEVICE_NAME}}"), after_len + 1);
            memcpy(pos, device_name, strlen(device_name));
            chunk_size = before_len + strlen(device_name) + after_len;
        }
        
        // Replace {{CURRENT_SSID}}
        if ((pos = strstr(chunk_buffer, "{{CURRENT_SSID}}")) != NULL) {
            size_t before_len = pos - chunk_buffer;
            size_t after_start = before_len + strlen("{{CURRENT_SSID}}");
            size_t after_len = chunk_size - after_start;
            
            memmove(pos + strlen(ssid), pos + strlen("{{CURRENT_SSID}}"), after_len + 1);
            memcpy(pos, ssid, strlen(ssid));
            chunk_size = before_len + strlen(ssid) + after_len;
        }
        
        // Replace {{CONNECTION_STATUS}}
        if ((pos = strstr(chunk_buffer, "{{CONNECTION_STATUS}}")) != NULL) {
            size_t before_len = pos - chunk_buffer;
            size_t after_start = before_len + strlen("{{CONNECTION_STATUS}}");
            size_t after_len = chunk_size - after_start;
            
            memmove(pos + strlen(status), pos + strlen("{{CONNECTION_STATUS}}"), after_len + 1);
            memcpy(pos, status, strlen(status));
            chunk_size = before_len + strlen(status) + after_len;
        }
        
        // Handle conditional sections for SPDIF and USB
        // Simplified: Remove or keep sections based on compile flags
#ifdef IS_SPDIF
        // Keep SPDIF sections - just remove the tags
        while ((pos = strstr(chunk_buffer, "{{#IS_SPDIF}}")) != NULL) {
            memmove(pos, pos + strlen("{{#IS_SPDIF}}"), strlen(pos + strlen("{{#IS_SPDIF}}")) + 1);
            chunk_size -= strlen("{{#IS_SPDIF}}");
        }
        while ((pos = strstr(chunk_buffer, "{{/IS_SPDIF}}")) != NULL) {
            memmove(pos, pos + strlen("{{/IS_SPDIF}}"), strlen(pos + strlen("{{/IS_SPDIF}}")) + 1);
            chunk_size -= strlen("{{/IS_SPDIF}}");
        }
#else
        // Remove SPDIF sections entirely
        {
            char *start_tag, *end_tag;
            while ((start_tag = strstr(chunk_buffer, "{{#IS_SPDIF}}")) != NULL &&
                   (end_tag = strstr(start_tag, "{{/IS_SPDIF}}")) != NULL) {
                end_tag += strlen("{{/IS_SPDIF}}");
                memmove(start_tag, end_tag, strlen(end_tag) + 1);
                chunk_size -= (end_tag - start_tag);
            }
        }
#endif

#ifdef IS_USB
        // Keep USB sections - just remove the tags
        while ((pos = strstr(chunk_buffer, "{{#IS_USB}}")) != NULL) {
            memmove(pos, pos + strlen("{{#IS_USB}}"), strlen(pos + strlen("{{#IS_USB}}")) + 1);
            chunk_size -= strlen("{{#IS_USB}}");
        }
        while ((pos = strstr(chunk_buffer, "{{/IS_USB}}")) != NULL) {
            memmove(pos, pos + strlen("{{/IS_USB}}"), strlen(pos + strlen("{{/IS_USB}}")) + 1);
            chunk_size -= strlen("{{/IS_USB}}");
        }
#else
        // Remove USB sections entirely
        {
            char *start_tag, *end_tag;
            while ((start_tag = strstr(chunk_buffer, "{{#IS_USB}}")) != NULL &&
                   (end_tag = strstr(start_tag, "{{/IS_USB}}")) != NULL) {
                end_tag += strlen("{{/IS_USB}}");
                memmove(start_tag, end_tag, strlen(end_tag) + 1);
                chunk_size -= (end_tag - start_tag);
            }
        }
#endif
        
        // Send this chunk
        ret = httpd_resp_send_chunk(req, chunk_buffer, chunk_size);
        
        offset += CHUNK_SIZE; // Move by original chunk size, not modified size
    }
    
    // Send final chunk to indicate end of response
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    
    free(chunk_buffer);
    
    #undef CHUNK_SIZE
    return ret;
}

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
 * GET handler for scanning available WiFi networks
 */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /scan");

    // Scan for networks
    wifi_network_info_t networks[MAX_SCAN_RESULTS];
    size_t networks_found = 0;

    esp_err_t ret = wifi_manager_scan_networks(networks, MAX_SCAN_RESULTS, &networks_found);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to scan networks");
        return ESP_FAIL;
    }

    // Create JSON response
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Deduplicate networks in-place - O(n²) but with no extra memory usage
    // Mark duplicates by setting their SSID to empty string
    for (size_t i = 0; i < networks_found; i++) {
        // Skip if this network is already marked as a duplicate or has an empty SSID
        if (strlen(networks[i].ssid) == 0) {
            continue;
        }

        // Look for duplicates of this network
        for (size_t j = i + 1; j < networks_found; j++) {
            if (strlen(networks[j].ssid) > 0 && strcmp(networks[i].ssid, networks[j].ssid) == 0) {
                // Same SSID - keep the one with stronger signal
                if (networks[j].rssi > networks[i].rssi) {
                    // j has stronger signal, copy to i and mark j as duplicate
                    networks[i].rssi = networks[j].rssi;
                    networks[i].authmode = networks[j].authmode;
                    networks[j].ssid[0] = '\0'; // Mark as duplicate
                } else {
                    // i has stronger signal, mark j as duplicate
                    networks[j].ssid[0] = '\0'; // Mark as duplicate
                }
            }
        }
    }

    // Add networks to JSON array (only the non-duplicates)
    for (size_t i = 0; i < networks_found; i++) {
        // Skip networks marked as duplicates (empty SSID)
        if (strlen(networks[i].ssid) == 0) {
            continue;
        }

        cJSON *network = cJSON_CreateObject();
        if (!network) {
            continue;
        }

        cJSON_AddStringToObject(network, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", networks[i].rssi);
        cJSON_AddNumberToObject(network, "auth", networks[i].authmode);

        cJSON_AddItemToArray(root, network);
    }

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    ret = httpd_resp_send(req, json_str, strlen(json_str));

    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);

    return ret;
}

// External declarations for deep sleep functionality
extern void enter_deep_sleep_mode(void);
#ifdef IS_USB
extern uac_host_device_handle_t s_spk_dev_handle; // DAC handle from usb_audio_player_main.c
#endif

// SPDIF functions are now handled through lifecycle_manager

/**
 * POST handler for connecting to a WiFi network
 */
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /connect");

    // Get content length
    size_t content_len = req->content_len;
    if (content_len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    // Read the data
    char buf[512];
    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[content_len] = '\0';

    // Parse the form data
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};

    char *param = strstr(buf, "ssid=");
    if (param) {
        param += 5; // Skip "ssid="
        char *end = strchr(param, '&');
        if (end) {
            *end = '\0';
        }

        // URL decode
        char *decoded = malloc(strlen(param) + 1);
        if (!decoded) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        // Simple URL decoding
        char *src = param;
        char *dst = decoded;
        while (*src) {
            if (*src == '+') {
                *dst = ' ';
            } else if (*src == '%' && src[1] && src[2]) {
                // Convert hex %xx to character
                char hex[3] = {src[1], src[2], 0};
                *dst = (char)strtol(hex, NULL, 16);
                src += 2;
            } else {
                *dst = *src;
            }
            src++;
            dst++;
        }
        *dst = '\0';

        strncpy(ssid, decoded, WIFI_SSID_MAX_LENGTH);
        free(decoded);

        if (end) {
            *end = '&';
        }
    }

    param = strstr(buf, "password=");
    if (param) {
        param += 9; // Skip "password="
        char *end = strchr(param, '&');
        if (end) {
            *end = '\0';
        }

        // URL decode
        char *decoded = malloc(strlen(param) + 1);
        if (!decoded) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        // Simple URL decoding
        char *src = param;
        char *dst = decoded;
        while (*src) {
            if (*src == '+') {
                *dst = ' ';
            } else if (*src == '%' && src[1] && src[2]) {
                // Convert hex %xx to character
                char hex[3] = {src[1], src[2], 0};
                *dst = (char)strtol(hex, NULL, 16);
                src += 2;
            } else {
                *dst = *src;
            }
            src++;
            dst++;
        }
        *dst = '\0';

        strncpy(password, decoded, WIFI_PASSWORD_MAX_LENGTH);
        free(decoded);
    }

    // Check if SSID is provided
    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    // Check if this is the first time an SSID is being saved
    bool first_time_config = !wifi_manager_has_credentials();

    // Save the credentials to NVS and try to connect
    wifi_manager_connect(ssid, password);

    // Respond with success even if connection failed
    // The client will be redirected when the device restarts or changes mode
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");

    // If this is the first time setting up WiFi and no DAC is connected,
    // put the device into deep sleep mode after a short delay
    if (first_time_config) {
        ESP_LOGI(TAG, "First-time WiFi configuration detected");

#ifdef IS_USB
        // Check if a DAC is connected (s_spk_dev_handle is NULL when no DAC is connected)
        if (s_spk_dev_handle == NULL) {
            ESP_LOGI(TAG, "No DAC connected after initial WiFi setup, preparing for deep sleep");
            // Give a small delay for web request to complete and logs to be sent
            vTaskDelay(pdMS_TO_TICKS(2000));
            // Go to deep sleep
        } else {
            ESP_LOGI(TAG, "DAC is connected, staying awake after WiFi setup");
        }
#else
        // If USB support is not enabled, we don't check for DAC
        ESP_LOGI(TAG, "USB support not enabled, staying awake after WiFi setup");
#endif
    }

    return ESP_OK;
}

/**
 * GET handler for triggering immediate mDNS discovery scan
 */
static esp_err_t discover_devices_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/discover_devices");

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // This endpoint now just triggers a scan and returns the results, same as /scream_devices.
    return scream_devices_handler(req);
}

/**
 * GET handler for retrieving list of discovered Scream devices
 */
static esp_err_t scream_devices_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling request for /api/scream_devices");

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Get the current list of discovered devices from the continuous discovery service
    discovered_device_t devices[MAX_DISCOVERED_DEVICES];
    size_t device_count = 0;
    esp_err_t err = mdns_discovery_get_devices(devices, MAX_DISCOVERED_DEVICES, &device_count);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get discovered devices: 0x%x", err);
        device_count = 0;
    }

    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "device_count", device_count);
    cJSON *devices_array = cJSON_AddArrayToObject(root, "devices");

    for (size_t i = 0; i < device_count; i++) {
        cJSON *device_obj = cJSON_CreateObject();
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&devices[i].ip_addr));

        cJSON_AddStringToObject(device_obj, "hostname", devices[i].hostname);
        cJSON_AddStringToObject(device_obj, "ip_address", ip_str);
        cJSON_AddNumberToObject(device_obj, "port", devices[i].port);
        cJSON_AddItemToArray(devices_array, device_obj);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/**
 * POST handler for selecting a Scream device as destination
 */

/**
 * POST handler for resetting WiFi configuration
 */
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /reset");

    // Clear the stored credentials
    esp_err_t err = wifi_manager_clear_credentials();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset WiFi configuration");
        return ESP_FAIL;
    }

    // Reset app configuration to defaults
    err = lifecycle_reset_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reset app configuration: 0x%x", err);
        // Continue anyway, WiFi reset is more important
    } else {
        ESP_LOGI(TAG, "App configuration reset to defaults");
    }

    // Start AP mode
    wifi_manager_stop();
    wifi_manager_start();

    // Respond with success
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * Handler for starting pairing mode
 */
static esp_err_t start_pairing_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Starting pairing mode via API");
    ESP_LOGI(TAG, "[DEBUG] Request pointer: %p", req);
    
    // Check for NULL request pointer
    if (!req) {
        ESP_LOGE(TAG, "Request pointer is NULL in start_pairing_handler");
        return ESP_FAIL;
    }
    
    // Set CORS headers for OPTIONS request
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // Post event to lifecycle manager to start pairing
    esp_err_t ret = lifecycle_manager_post_event(LIFECYCLE_EVENT_START_PAIRING);
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char json_response[128];
    if (ret == ESP_OK) {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"success\",\"message\":\"Pairing mode started\"}");
        httpd_resp_send(req, json_response, strlen(json_response));
    } else {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"error\",\"message\":\"Failed to start pairing mode\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, json_response, strlen(json_response));
    }
    
    return ESP_OK;
}

/**
 * Handler for cancelling pairing mode
 */
static esp_err_t cancel_pairing_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Cancelling pairing mode via API");
    ESP_LOGI(TAG, "[DEBUG] Request pointer: %p", req);
    
    // Check for NULL request pointer
    if (!req) {
        ESP_LOGE(TAG, "Request pointer is NULL in cancel_pairing_handler");
        return ESP_FAIL;
    }
    
    // Set CORS headers for OPTIONS request
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // Post event to lifecycle manager to cancel pairing
    esp_err_t ret = lifecycle_manager_post_event(LIFECYCLE_EVENT_CANCEL_PAIRING);
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char json_response[128];
    if (ret == ESP_OK) {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"success\",\"message\":\"Pairing mode cancelled\"}");
        httpd_resp_send(req, json_response, strlen(json_response));
    } else {
        snprintf(json_response, sizeof(json_response),
                "{\"status\":\"error\",\"message\":\"Failed to cancel pairing mode\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, json_response, strlen(json_response));
    }
    
    return ESP_OK;
}

/**
 * GET handler for all other URIs (redirect to captive portal)
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
 * GET handler for connection status
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /status");
    bq25895_reset_watchdog();
    // Get connection status
    wifi_manager_state_t state = wifi_manager_get_state();

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add status string
    const char *status;
    switch (state) {
        case WIFI_MANAGER_STATE_CONNECTED:
            status = "Connected";
            break;
        case WIFI_MANAGER_STATE_CONNECTING:
            status = "Connecting...";
            break;
        case WIFI_MANAGER_STATE_CONNECTION_FAILED:
            status = "Connection failed";
            break;
        case WIFI_MANAGER_STATE_AP_MODE:
            status = "Access Point Mode";
            break;
        default:
            status = "Unknown";
            break;
    }
    cJSON_AddStringToObject(root, "status", status);

    // If connected, add the IP address
    if (state == WIFI_MANAGER_STATE_CONNECTED) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ip_str);
        }
    }

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));

    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);

    return ret;
}

/**
 * GET handler for device settings
 */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/settings");

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Network settings - use lifecycle getter functions
    cJSON_AddNumberToObject(root, "port", lifecycle_get_port());
    cJSON_AddStringToObject(root, "hostname", lifecycle_get_hostname());
    cJSON_AddStringToObject(root, "ap_ssid", lifecycle_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_password", lifecycle_get_ap_password());
    cJSON_AddBoolToObject(root, "hide_ap_when_connected", lifecycle_get_hide_ap_when_connected());

    // Buffer settings
    cJSON_AddNumberToObject(root, "initial_buffer_size", lifecycle_get_initial_buffer_size());
    cJSON_AddNumberToObject(root, "buffer_grow_step_size", lifecycle_get_buffer_grow_step_size());
    cJSON_AddNumberToObject(root, "max_buffer_size", lifecycle_get_max_buffer_size());
    cJSON_AddNumberToObject(root, "max_grow_size", lifecycle_get_max_grow_size());

    // Audio settings
    cJSON_AddNumberToObject(root, "sample_rate", lifecycle_get_sample_rate());
    cJSON_AddNumberToObject(root, "bit_depth", lifecycle_get_bit_depth());
    cJSON_AddNumberToObject(root, "volume", lifecycle_get_volume());
    
    // Device mode
    device_mode_t mode = lifecycle_get_device_mode();
    cJSON_AddNumberToObject(root, "device_mode", mode);
    
    // Legacy compatibility - still send the boolean fields based on mode
    cJSON_AddBoolToObject(root, "enable_usb_sender", mode == MODE_SENDER_USB);
    cJSON_AddBoolToObject(root, "enable_spdif_sender", mode == MODE_SENDER_SPDIF);

    // SPDIF settings (only relevant when IS_SPDIF is defined)
#ifdef IS_SPDIF
    cJSON_AddNumberToObject(root, "spdif_data_pin", lifecycle_get_spdif_data_pin());
#endif

    // Sender settings
    cJSON_AddStringToObject(root, "sender_destination_ip", lifecycle_get_sender_destination_ip());
    cJSON_AddNumberToObject(root, "sender_destination_port", lifecycle_get_sender_destination_port());
    
    // Sleep settings
    cJSON_AddNumberToObject(root, "silence_threshold_ms", lifecycle_get_silence_threshold_ms());
    cJSON_AddNumberToObject(root, "network_check_interval_ms", lifecycle_get_network_check_interval_ms());
    cJSON_AddNumberToObject(root, "activity_threshold_packets", lifecycle_get_activity_threshold_packets());
    cJSON_AddNumberToObject(root, "silence_amplitude_threshold", lifecycle_get_silence_amplitude_threshold());
    cJSON_AddNumberToObject(root, "network_inactivity_timeout_ms", lifecycle_get_network_inactivity_timeout_ms());

    // Use Direct Write
    cJSON_AddBoolToObject(root, "use_direct_write", lifecycle_get_use_direct_write());
    
    // Setup wizard status
    cJSON_AddBoolToObject(root, "setup_wizard_completed", lifecycle_get_setup_wizard_completed());

    // Current WiFi SSID and password (if connected)
    char current_ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char current_password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};

    // Get both SSID and password using the existing function
    if (wifi_manager_get_credentials(current_ssid, sizeof(current_ssid),
                                     current_password, sizeof(current_password)) == ESP_OK) {
        if (strlen(current_ssid) > 0) {
            cJSON_AddStringToObject(root, "ssid", current_ssid);
            cJSON_AddStringToObject(root, "password", current_password);
        }
    }

    // Device capabilities (compile-time flags)
#ifdef IS_USB
    cJSON_AddBoolToObject(root, "has_usb_capability", true);
#else
    cJSON_AddBoolToObject(root, "has_usb_capability", false);
#endif

#ifdef IS_SPDIF
    cJSON_AddBoolToObject(root, "has_spdif_capability", true);
#else
    cJSON_AddBoolToObject(root, "has_spdif_capability", false);
#endif

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "application/json");

    // Send the response
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));

    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);

    return ret;
}

/**
 * POST handler for updating device settings
 */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /api/settings");

    // Get content length
    size_t content_len = req->content_len;
    if (content_len >= 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    // Read the data
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[content_len] = '\0';

    // Parse the JSON data
    cJSON *root = cJSON_Parse(buf);
    free(buf); // We don't need the raw data anymore

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Prepare batch update structure
    lifecycle_config_update_t updates = {0};

    // Parse and prepare updates for port
    cJSON *port = cJSON_GetObjectItem(root, "port");
    if (port && cJSON_IsNumber(port)) {
        updates.update_port = true;
        updates.port = (uint16_t)port->valueint;
    }

    // Hostname
    cJSON *hostname = cJSON_GetObjectItem(root, "hostname");
    if (hostname && cJSON_IsString(hostname)) {
        updates.update_hostname = true;
        updates.hostname = hostname->valuestring;
    }

    // WiFi AP SSID
    cJSON *ap_ssid = cJSON_GetObjectItem(root, "ap_ssid");
    if (ap_ssid && cJSON_IsString(ap_ssid)) {
        updates.update_ap_ssid = true;
        updates.ap_ssid = ap_ssid->valuestring;
    }

    // WiFi AP password
    cJSON *ap_password = cJSON_GetObjectItem(root, "ap_password");
    if (ap_password && cJSON_IsString(ap_password)) {
        updates.update_ap_password = true;
        updates.ap_password = ap_password->valuestring;
    }

    // Hide AP when connected setting
    cJSON *hide_ap_when_connected = cJSON_GetObjectItem(root, "hide_ap_when_connected");
    if (hide_ap_when_connected && cJSON_IsBool(hide_ap_when_connected)) {
        updates.update_hide_ap_when_connected = true;
        updates.hide_ap_when_connected = cJSON_IsTrue(hide_ap_when_connected);
    }

    // Buffer settings
    cJSON *initial_buffer_size = cJSON_GetObjectItem(root, "initial_buffer_size");
    if (initial_buffer_size && cJSON_IsNumber(initial_buffer_size)) {
        updates.update_initial_buffer_size = true;
        updates.initial_buffer_size = (uint8_t)initial_buffer_size->valueint;
    }

    cJSON *buffer_grow_step_size = cJSON_GetObjectItem(root, "buffer_grow_step_size");
    if (buffer_grow_step_size && cJSON_IsNumber(buffer_grow_step_size)) {
        updates.update_buffer_grow_step_size = true;
        updates.buffer_grow_step_size = (uint8_t)buffer_grow_step_size->valueint;
    }

    cJSON *max_buffer_size = cJSON_GetObjectItem(root, "max_buffer_size");
    if (max_buffer_size && cJSON_IsNumber(max_buffer_size)) {
        updates.update_max_buffer_size = true;
        updates.max_buffer_size = (uint8_t)max_buffer_size->valueint;
    }

    cJSON *max_grow_size = cJSON_GetObjectItem(root, "max_grow_size");
    if (max_grow_size && cJSON_IsNumber(max_grow_size)) {
        updates.update_max_grow_size = true;
        updates.max_grow_size = (uint8_t)max_grow_size->valueint;
    }

    // Audio settings
    // Sample rate is handled separately after batch update
    cJSON *sample_rate = cJSON_GetObjectItem(root, "sample_rate");

    cJSON *bit_depth = cJSON_GetObjectItem(root, "bit_depth");
    if (bit_depth && cJSON_IsNumber(bit_depth)) {
        // Bit depth is fixed at 16, nothing to update
    }

    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (volume && cJSON_IsNumber(volume)) {
        updates.update_volume = true;
        updates.volume = (float)volume->valuedouble;
    }

    // Sleep settings
    cJSON *silence_threshold_ms = cJSON_GetObjectItem(root, "silence_threshold_ms");
    if (silence_threshold_ms && cJSON_IsNumber(silence_threshold_ms)) {
        updates.update_silence_threshold_ms = true;
        updates.silence_threshold_ms = (uint32_t)silence_threshold_ms->valueint;
    }

    cJSON *network_check_interval_ms = cJSON_GetObjectItem(root, "network_check_interval_ms");
    if (network_check_interval_ms && cJSON_IsNumber(network_check_interval_ms)) {
        updates.update_network_check_interval_ms = true;
        updates.network_check_interval_ms = (uint32_t)network_check_interval_ms->valueint;
    }

    cJSON *activity_threshold_packets = cJSON_GetObjectItem(root, "activity_threshold_packets");
    if (activity_threshold_packets && cJSON_IsNumber(activity_threshold_packets)) {
        updates.update_activity_threshold_packets = true;
        updates.activity_threshold_packets = (uint8_t)activity_threshold_packets->valueint;
    }

    cJSON *silence_amplitude_threshold = cJSON_GetObjectItem(root, "silence_amplitude_threshold");
    if (silence_amplitude_threshold && cJSON_IsNumber(silence_amplitude_threshold)) {
        updates.update_silence_amplitude_threshold = true;
        updates.silence_amplitude_threshold = (uint16_t)silence_amplitude_threshold->valueint;
    }

    cJSON *network_inactivity_timeout_ms = cJSON_GetObjectItem(root, "network_inactivity_timeout_ms");
    if (network_inactivity_timeout_ms && cJSON_IsNumber(network_inactivity_timeout_ms)) {
        updates.update_network_inactivity_timeout_ms = true;
        updates.network_inactivity_timeout_ms = (uint32_t)network_inactivity_timeout_ms->valueint;
    }

    // Device mode setting (new preferred way)
    cJSON *device_mode = cJSON_GetObjectItem(root, "device_mode");
    if (device_mode && cJSON_IsNumber(device_mode)) {
        device_mode_t mode = (device_mode_t)device_mode->valueint;
        if (mode >= MODE_RECEIVER_USB && mode <= MODE_SENDER_SPDIF) {
            updates.update_device_mode = true;
            updates.device_mode = mode;
            ESP_LOGI(TAG, "Updating device_mode to: %d", mode);
        }
    }
    
    // Legacy USB Sender settings (for backward compatibility)
    cJSON *enable_usb_sender = cJSON_GetObjectItem(root, "enable_usb_sender");
    if (enable_usb_sender && cJSON_IsBool(enable_usb_sender)) {
        if (!device_mode) {  // Only use if device_mode wasn't provided
            updates.update_enable_usb_sender = true;
            updates.enable_usb_sender = cJSON_IsTrue(enable_usb_sender);
            ESP_LOGI(TAG, "Updating USB sender enabled to: %d", updates.enable_usb_sender);
        }
    }

    cJSON *sender_destination_ip = cJSON_GetObjectItem(root, "sender_destination_ip");
    if (sender_destination_ip && cJSON_IsString(sender_destination_ip)) {
        updates.update_sender_destination_ip = true;
        updates.sender_destination_ip = sender_destination_ip->valuestring;
        ESP_LOGI(TAG, "Updating sender destination IP to: %s", updates.sender_destination_ip);
    }

    cJSON *sender_destination_port = cJSON_GetObjectItem(root, "sender_destination_port");
    if (sender_destination_port && cJSON_IsNumber(sender_destination_port)) {
        updates.update_sender_destination_port = true;
        updates.sender_destination_port = (uint16_t)sender_destination_port->valueint;
        ESP_LOGI(TAG, "Updating sender destination port to: %d", updates.sender_destination_port);
    }

    // SPDIF settings
#ifdef IS_SPDIF
    cJSON *spdif_data_pin = cJSON_GetObjectItem(root, "spdif_data_pin");
    if (spdif_data_pin && cJSON_IsNumber(spdif_data_pin)) {
        // Limit the pin number to valid GPIO range (0-39 for ESP32)
        uint8_t pin = (uint8_t)spdif_data_pin->valueint;
        if (pin <= 39) {
            updates.update_spdif_data_pin = true;
            updates.spdif_data_pin = pin;
        }
    }
#endif

    // Legacy SPDIF sender setting (for backward compatibility)
#ifdef IS_SPDIF
    cJSON *enable_spdif_sender = cJSON_GetObjectItem(root, "enable_spdif_sender");
    if (enable_spdif_sender && cJSON_IsBool(enable_spdif_sender)) {
        if (!device_mode) {  // Only use if device_mode wasn't provided
            updates.update_enable_spdif_sender = true;
            updates.enable_spdif_sender = cJSON_IsTrue(enable_spdif_sender);
            ESP_LOGI(TAG, "Updating S/PDIF sender enabled to: %d", updates.enable_spdif_sender);
        }
    }
#endif

    // Handle direct write setting
    cJSON *use_direct_write = cJSON_GetObjectItem(root, "use_direct_write");
    ESP_LOGI(TAG, "Got use_direct_write from JSON: %p", use_direct_write);
    if (use_direct_write) {
        ESP_LOGI(TAG, "use_direct_write type: %d", use_direct_write->type);
    }
    if (use_direct_write && cJSON_IsBool(use_direct_write)) {
        bool old_value = lifecycle_get_use_direct_write();
        updates.update_use_direct_write = true;
        updates.use_direct_write = cJSON_IsTrue(use_direct_write);
        ESP_LOGI(TAG, "Direct write mode changed from %d to %d", old_value, updates.use_direct_write);
    } else {
        ESP_LOGW(TAG, "Invalid or missing use_direct_write in request");
    }
    
    // Handle setup wizard completed flag
    cJSON *setup_wizard_completed = cJSON_GetObjectItem(root, "setup_wizard_completed");
    esp_err_t err = ESP_OK;
    if (setup_wizard_completed && cJSON_IsBool(setup_wizard_completed)) {
        bool completed = cJSON_IsTrue(setup_wizard_completed);
        err = lifecycle_set_setup_wizard_completed(completed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set setup wizard completed status: %s", esp_err_to_name(err));
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set setup wizard status");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Setup wizard completed status changed to %d", completed);
    }

    // Apply batch updates through lifecycle_manager
    err = lifecycle_update_config_batch(&updates);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update configuration");
        return ESP_FAIL;
    }

    // Handle sample rate separately if changed
    if (sample_rate && cJSON_IsNumber(sample_rate)) {
        uint32_t new_rate = (uint32_t)sample_rate->valueint;
        uint32_t current_rate = lifecycle_get_sample_rate();
        if (new_rate != current_rate) {
            ESP_LOGI(TAG, "Sample rate change requested: %lu Hz", new_rate);
            err = lifecycle_manager_change_sample_rate(new_rate);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to change sample rate: 0x%x", err);
            }
        }
    }

    // Post configuration changed event to lifecycle manager
    // The unified handler will apply all immediate changes
    ESP_LOGI(TAG, "Configuration saved, posting event to lifecycle manager");
    lifecycle_manager_post_event(LIFECYCLE_EVENT_CONFIGURATION_CHANGED);

    cJSON_Delete(root);

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Settings saved successfully\"}");

    return ESP_OK;
}

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
 * OTA Upload handler - receives firmware binary via HTTP POST
 * Handles chunked upload with progress tracking
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA upload request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Content-Length");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Check if OTA is already in progress
    if (ota_manager_is_in_progress()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"OTA update already in progress\"}");
        return ESP_FAIL;
    }

    // Get content length
    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"No content provided\"}");
        return ESP_FAIL;
    }

    // Check if firmware size is within limits
    if (content_len > OTA_MAX_FIRMWARE_SIZE) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Firmware size exceeds maximum allowed size\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA update, firmware size: %zu bytes", content_len);

    // Start OTA update
    esp_err_t err = ota_manager_start(content_len, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA: 0x%x", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "{\"error\":\"Failed to start OTA: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, error_msg);
        return ESP_FAIL;
    }

    // Allocate buffer for receiving data
    const size_t buffer_size = 4096;
    uint8_t *buffer = malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        ota_manager_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    size_t received = 0;
    int remaining = content_len;
    bool ota_failed = false;

    // Receive and write firmware data in chunks
    while (remaining > 0) {
        // Receive chunk
        int recv_len = httpd_req_recv(req, (char *)buffer,
                                     remaining < buffer_size ? remaining : buffer_size);
        
        if (recv_len < 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Socket timeout during OTA upload");
                httpd_resp_send_408(req);
            } else {
                ESP_LOGE(TAG, "Failed to receive data: %d", recv_len);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            }
            ota_failed = true;
            break;
        }

        // Write to OTA partition
        err = ota_manager_write(buffer, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: 0x%x", err);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                    "{\"error\":\"Failed to write OTA data: %s\"}", esp_err_to_name(err));
            httpd_resp_sendstr(req, error_msg);
            ota_failed = true;
            break;
        }

        received += recv_len;
        remaining -= recv_len;

        // Log progress every 10%
        uint8_t progress = (received * 100) / content_len;
        static uint8_t last_progress = 0;
        if (progress >= last_progress + 10) {
            ESP_LOGI(TAG, "OTA progress: %d%% (%zu/%zu bytes)", progress, received, content_len);
            last_progress = progress;
        }
    }

    free(buffer);

    // Handle completion or failure
    if (ota_failed) {
        ota_manager_abort();
        return ESP_FAIL;
    }

    // Complete OTA update
    err = ota_manager_complete();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to complete OTA: 0x%x", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                "{\"error\":\"Failed to complete OTA: %s\",\"message\":\"%s\"}",
                esp_err_to_name(err), ota_manager_get_error_message());
        httpd_resp_sendstr(req, error_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update completed successfully");

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"OTA update completed. Device will restart.\"}");

    // Schedule restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/**
 * OTA Status handler - returns current OTA state and progress
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA status request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Get OTA status
    ota_status_t status;
    esp_err_t err = ota_manager_get_status(&status);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get OTA status");
        return ESP_FAIL;
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add status fields
    const char *state_str;
    switch (status.state) {
        case OTA_STATE_IDLE:
            state_str = "idle";
            break;
        case OTA_STATE_CHECKING:
            state_str = "checking";
            break;
        case OTA_STATE_DOWNLOADING:
            state_str = "downloading";
            break;
        case OTA_STATE_VERIFYING:
            state_str = "verifying";
            break;
        case OTA_STATE_APPLYING:
            state_str = "applying";
            break;
        case OTA_STATE_SUCCESS:
            state_str = "success";
            break;
        case OTA_STATE_ERROR:
            state_str = "error";
            break;
        case OTA_STATE_ROLLBACK:
            state_str = "rollback";
            break;
        default:
            state_str = "unknown";
            break;
    }

    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddNumberToObject(root, "error_code", status.error_code);
    cJSON_AddNumberToObject(root, "progress", status.progress_percent);
    cJSON_AddNumberToObject(root, "total_size", status.total_size);
    cJSON_AddNumberToObject(root, "received_size", status.received_size);
    
    if (status.error_code != OTA_ERR_NONE) {
        cJSON_AddStringToObject(root, "error_message", status.error_message);
    }
    
    if (status.state != OTA_STATE_IDLE) {
        cJSON_AddNumberToObject(root, "duration_ms", status.update_duration_ms);
    }
    
    cJSON_AddStringToObject(root, "current_version", status.firmware_version);
    
    if (strlen(status.new_version) > 0) {
        cJSON_AddStringToObject(root, "new_version", status.new_version);
    }

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/**
 * OTA Version handler - returns current firmware version and partition info
 */
static esp_err_t ota_version_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA version request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add version information
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION_STRING);
    cJSON_AddStringToObject(root, "version_full", FIRMWARE_VERSION_FULL);
    cJSON_AddNumberToObject(root, "version_major", FIRMWARE_VERSION_MAJOR);
    cJSON_AddNumberToObject(root, "version_minor", FIRMWARE_VERSION_MINOR);
    cJSON_AddNumberToObject(root, "version_patch", FIRMWARE_VERSION_PATCH);
    cJSON_AddNumberToObject(root, "build_number", FIRMWARE_BUILD_NUMBER);
    
    // Add build information
    cJSON_AddStringToObject(root, "app_name", FIRMWARE_APP_NAME);
    cJSON_AddStringToObject(root, "platform", FIRMWARE_PLATFORM);
    cJSON_AddStringToObject(root, "build_date", FIRMWARE_BUILD_DATE);
    cJSON_AddStringToObject(root, "build_time", FIRMWARE_BUILD_TIME);
    cJSON_AddStringToObject(root, "build_type", FIRMWARE_BUILD_TYPE);
    cJSON_AddStringToObject(root, "git_commit", FIRMWARE_GIT_COMMIT);
    cJSON_AddStringToObject(root, "git_branch", FIRMWARE_GIT_BRANCH);

    // Add partition information
    const esp_partition_t *running_partition = ota_manager_get_running_partition();
    const esp_partition_t *update_partition = ota_manager_get_update_partition();

    if (running_partition) {
        cJSON *running = cJSON_CreateObject();
        cJSON_AddStringToObject(running, "label", running_partition->label);
        cJSON_AddNumberToObject(running, "address", running_partition->address);
        cJSON_AddNumberToObject(running, "size", running_partition->size);
        cJSON_AddNumberToObject(running, "type", running_partition->type);
        cJSON_AddNumberToObject(running, "subtype", running_partition->subtype);
        cJSON_AddItemToObject(root, "running_partition", running);
    }

    if (update_partition) {
        cJSON *update = cJSON_CreateObject();
        cJSON_AddStringToObject(update, "label", update_partition->label);
        cJSON_AddNumberToObject(update, "address", update_partition->address);
        cJSON_AddNumberToObject(update, "size", update_partition->size);
        cJSON_AddNumberToObject(update, "type", update_partition->type);
        cJSON_AddNumberToObject(update, "subtype", update_partition->subtype);
        cJSON_AddItemToObject(root, "update_partition", update);
    }

    // Add OTA capabilities
    cJSON_AddBoolToObject(root, "can_rollback", ota_manager_can_rollback());
    cJSON_AddNumberToObject(root, "max_firmware_size", OTA_MAX_FIRMWARE_SIZE);

    // Get firmware info
    ota_firmware_info_t fw_info;
    if (ota_manager_get_firmware_info(&fw_info) == ESP_OK) {
        cJSON *firmware_info = cJSON_CreateObject();
        cJSON_AddStringToObject(firmware_info, "version", fw_info.version);
        cJSON_AddNumberToObject(firmware_info, "size", fw_info.size);
        cJSON_AddNumberToObject(firmware_info, "crc32", fw_info.crc32);
        cJSON_AddNumberToObject(firmware_info, "build_timestamp", fw_info.build_timestamp);
        cJSON_AddStringToObject(firmware_info, "build_hash", fw_info.build_hash);
        cJSON_AddBoolToObject(firmware_info, "secure_signed", fw_info.secure_signed);
        cJSON_AddItemToObject(root, "firmware_info", firmware_info);
    }

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/**
 * GET handler for retrieving system logs from the log buffer
 */
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/logs");

    // Parse query parameters
    char query_buf[256] = {0};
    size_t buf_len = sizeof(query_buf);
    httpd_req_get_url_query_str(req, query_buf, buf_len);

    // Parse 'lines' parameter (default: 50, max: 200)
    int lines_requested = 50;
    char param_buf[32] = {0};
    if (httpd_query_key_value(query_buf, "lines", param_buf, sizeof(param_buf)) == ESP_OK) {
        int parsed_lines = atoi(param_buf);
        if (parsed_lines > 0 && parsed_lines <= 200) {
            lines_requested = parsed_lines;
        }
    }

    // Parse 'clear' parameter
    bool clear_after_read = false;
    if (httpd_query_key_value(query_buf, "clear", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strcmp(param_buf, "true") == 0 || strcmp(param_buf, "1") == 0) {
            clear_after_read = true;
        }
    }

    // Parse 'level' parameter for display filtering (ERROR, WARN, INFO, DEBUG, VERBOSE)
    esp_log_level_t display_min_level = ESP_LOG_VERBOSE;  // Default to show all
    if (httpd_query_key_value(query_buf, "level", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strcasecmp(param_buf, "ERROR") == 0) {
            display_min_level = ESP_LOG_ERROR;
        } else if (strcasecmp(param_buf, "WARN") == 0) {
            display_min_level = ESP_LOG_WARN;
        } else if (strcasecmp(param_buf, "INFO") == 0) {
            display_min_level = ESP_LOG_INFO;
        } else if (strcasecmp(param_buf, "DEBUG") == 0) {
            display_min_level = ESP_LOG_DEBUG;
        } else if (strcasecmp(param_buf, "VERBOSE") == 0) {
            display_min_level = ESP_LOG_VERBOSE;
        }
        ESP_LOGI(TAG, "Display filter level set to: %d", display_min_level);
    }

    // Parse 'capture_level' parameter to change what gets stored in buffer
    if (httpd_query_key_value(query_buf, "capture_level", param_buf, sizeof(param_buf)) == ESP_OK) {
        esp_log_level_t new_capture_level = ESP_LOG_INFO;  // Default
        if (strcasecmp(param_buf, "ERROR") == 0) {
            new_capture_level = ESP_LOG_ERROR;
        } else if (strcasecmp(param_buf, "WARN") == 0) {
            new_capture_level = ESP_LOG_WARN;
        } else if (strcasecmp(param_buf, "INFO") == 0) {
            new_capture_level = ESP_LOG_INFO;
        } else if (strcasecmp(param_buf, "DEBUG") == 0) {
            new_capture_level = ESP_LOG_DEBUG;
        } else if (strcasecmp(param_buf, "VERBOSE") == 0) {
            new_capture_level = ESP_LOG_VERBOSE;
        }
        
        // Set the new capture level
        esp_err_t err = log_buffer_set_min_level(new_capture_level);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Changed log capture level to: %d", new_capture_level);
        } else {
            ESP_LOGW(TAG, "Failed to change log capture level: 0x%x", err);
        }
    }

    // Use a fixed reasonable buffer size to avoid memory issues
    // Maximum 8KB for log data to be conservative with ESP32 memory
    size_t max_read_size = 8192;  // 8KB fixed buffer
    
    // Get current buffer status
    size_t buffer_used = log_buffer_get_used();
    size_t buffer_size = log_buffer_get_size();
    bool has_overflowed = log_buffer_has_overflowed();
    esp_log_level_t current_capture_level = log_buffer_get_min_level();

    // Limit read size to what's actually available
    if (max_read_size > buffer_used) {
        max_read_size = buffer_used;
    }

    // Allocate buffer for reading logs
    char *log_data = malloc(max_read_size + 1);
    if (!log_data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Read logs from buffer (use peek_latest to get newest logs)
    size_t bytes_read;
    if (clear_after_read) {
        // For clear, we still need to read all and clear, but we'll read the latest first
        bytes_read = log_buffer_peek_latest(log_data, max_read_size);
        log_buffer_clear();  // Clear the entire buffer after reading
        ESP_LOGI(TAG, "Read latest %zu bytes and cleared log buffer", bytes_read);
    } else {
        bytes_read = log_buffer_peek_latest(log_data, max_read_size);
        ESP_LOGD(TAG, "Peeked latest %zu bytes from log buffer", bytes_read);
    }
    log_data[bytes_read] = '\0';

    // Apply display-time level filtering if needed
    if (display_min_level < ESP_LOG_VERBOSE && bytes_read > 0) {
        // Filter logs line by line based on log level prefix
        char *filtered_data = malloc(max_read_size + 1);
        if (filtered_data) {
            size_t filtered_pos = 0;
            char *line_start = log_data;
            char *line_end;
            
            while ((line_end = strchr(line_start, '\n')) != NULL) {
                *line_end = '\0';  // Temporarily null-terminate the line
                
                // Check if this line should be displayed based on level
                bool should_display = true;
                
                // Check for ESP-IDF log level prefixes (E/W/I/D/V)
                if (line_start[0] == 'E' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_ERROR >= display_min_level);
                } else if (line_start[0] == 'W' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_WARN >= display_min_level);
                } else if (line_start[0] == 'I' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_INFO >= display_min_level);
                } else if (line_start[0] == 'D' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_DEBUG >= display_min_level);
                } else if (line_start[0] == 'V' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_VERBOSE >= display_min_level);
                }
                
                // Add line to filtered output if it passes the filter
                if (should_display) {
                    size_t line_len = strlen(line_start);
                    if (filtered_pos + line_len + 1 < max_read_size) {
                        memcpy(filtered_data + filtered_pos, line_start, line_len);
                        filtered_pos += line_len;
                        filtered_data[filtered_pos++] = '\n';
                    }
                }
                
                *line_end = '\n';  // Restore the newline
                line_start = line_end + 1;
            }
            
            // Handle last line if it doesn't end with newline
            if (*line_start != '\0') {
                bool should_display = true;
                
                if (line_start[0] == 'E' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_ERROR >= display_min_level);
                } else if (line_start[0] == 'W' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_WARN >= display_min_level);
                } else if (line_start[0] == 'I' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_INFO >= display_min_level);
                } else if (line_start[0] == 'D' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_DEBUG >= display_min_level);
                } else if (line_start[0] == 'V' && line_start[1] == ' ' && line_start[2] == '(') {
                    should_display = (ESP_LOG_VERBOSE >= display_min_level);
                }
                
                if (should_display) {
                    size_t line_len = strlen(line_start);
                    if (filtered_pos + line_len < max_read_size) {
                        memcpy(filtered_data + filtered_pos, line_start, line_len);
                        filtered_pos += line_len;
                    }
                }
            }
            
            filtered_data[filtered_pos] = '\0';
            
            // Replace original data with filtered data
            memcpy(log_data, filtered_data, filtered_pos + 1);
            bytes_read = filtered_pos;
            free(filtered_data);
        }
    }

    // If we need to limit to a specific number of lines, process the data
    if (bytes_read > 0 && lines_requested < 200) {
        // Count lines from the end of the buffer to get the most recent ones
        int line_count = 0;
        char *line_start = NULL;
        
        // Start from the end and work backwards to find the nth line
        for (int i = bytes_read - 1; i >= 0; i--) {
            if (log_data[i] == '\n' || i == 0) {
                line_count++;
                if (line_count == lines_requested) {
                    line_start = (i == 0) ? &log_data[0] : &log_data[i + 1];
                    break;
                }
            }
        }
        
        // If we found the starting point for the requested lines
        if (line_start && line_start != log_data) {
            size_t offset = line_start - log_data;
            memmove(log_data, line_start, bytes_read - offset + 1);
            bytes_read = bytes_read - offset;
            log_data[bytes_read] = '\0';
        }
    }

    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(log_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Add log data and status to JSON
    cJSON_AddStringToObject(root, "logs", log_data);
    cJSON_AddBoolToObject(root, "overflow", has_overflowed);
    cJSON_AddNumberToObject(root, "buffer_used", buffer_used);
    cJSON_AddNumberToObject(root, "buffer_size", buffer_size);
    
    // Add current capture level to response
    const char *capture_level_str;
    switch (current_capture_level) {
        case ESP_LOG_ERROR:
            capture_level_str = "ERROR";
            break;
        case ESP_LOG_WARN:
            capture_level_str = "WARN";
            break;
        case ESP_LOG_INFO:
            capture_level_str = "INFO";
            break;
        case ESP_LOG_DEBUG:
            capture_level_str = "DEBUG";
            break;
        case ESP_LOG_VERBOSE:
            capture_level_str = "VERBOSE";
            break;
        default:
            capture_level_str = "UNKNOWN";
            break;
    }
    cJSON_AddStringToObject(root, "capture_level", capture_level_str);

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    free(log_data);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/**
 * OTA Rollback handler - performs manual rollback to previous firmware
 */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling OTA rollback request");

    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Check if rollback is possible
    if (!ota_manager_can_rollback()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Rollback not available. No previous firmware found.\"}");
        return ESP_FAIL;
    }

    // Perform rollback
    esp_err_t err = ota_manager_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to perform rollback: 0x%x", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "{\"error\":\"Failed to rollback: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, error_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Rollback initiated successfully");

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Rollback initiated. Device will restart with previous firmware.\"}");

    // Schedule restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}


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
    config.max_uri_handlers = 45;

    // Increase buffer size for requests
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;

    // Bind the server to ANY address instead of just the AP interface
    // This allows it to be accessible from both AP and STA interfaces
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.task_priority = 2;

    // You might need to modify sdkconfig to increase HTTPD_MAX_REQ_HDR_LEN and HTTPD_MAX_URI_LEN

    // Start the HTTP server
    esp_err_t ret = httpd_start(&s_httpd_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: 0x%x", ret);
        return ret;
    }

    // Define URI handlers
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t scan = {
        .uri       = "/scan",
        .method    = HTTP_GET,
        .handler   = scan_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t connect = {
        .uri       = "/connect",
        .method    = HTTP_POST,
        .handler   = connect_post_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t reset = {
        .uri       = "/reset",
        .method    = HTTP_POST,
        .handler   = reset_post_handler,
        .user_ctx  = NULL
    };

    // Special handlers for captive portal functionality
    httpd_uri_t redirect = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = redirect_get_handler,
        .user_ctx  = NULL
    };

    // Handle Apple captive portal detection
    httpd_uri_t apple_cna = {
        .uri       = "/hotspot-detect.html",
        .method    = HTTP_GET,
        .handler   = apple_cna_get_handler,
        .user_ctx  = NULL
    };

    // Register URI handlers
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &root));

    // Add handlers for CSS and JS files
    httpd_uri_t css = {
        .uri       = "/styles.css",
        .method    = HTTP_GET,
        .handler   = css_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &css));

    httpd_uri_t js = {
        .uri       = "/script.js",
        .method    = HTTP_GET,
        .handler   = js_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &js));

    // Register wizard file handlers
    httpd_uri_t wizard_html = {
        .uri       = "/wizard.html",
        .method    = HTTP_GET,
        .handler   = wizard_html_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &wizard_html));

    httpd_uri_t wizard_css = {
        .uri       = "/wizard.css",
        .method    = HTTP_GET,
        .handler   = wizard_css_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &wizard_css));

    httpd_uri_t wizard_js_uri = {
        .uri       = "/wizard.js",
        .method    = HTTP_GET,
        .handler   = wizard_js_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &wizard_js_uri));

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &scan));

    // Add status endpoint
    httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &status));

    // Add settings endpoints
    httpd_uri_t settings_get = {
        .uri       = "/api/settings",
        .method    = HTTP_GET,
        .handler   = settings_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &settings_get));

    httpd_uri_t settings_post = {
        .uri       = "/api/settings",
        .method    = HTTP_POST,
        .handler   = settings_post_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &settings_post));

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &connect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &reset));

    // Register BQ25895 handlers
    httpd_uri_t bq25895_html = {
        .uri       = "/bq25895",
        .method    = HTTP_GET,
        .handler   = bq25895_html_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_html));

    httpd_uri_t bq25895_css = {
        .uri       = "/bq25895/css",
        .method    = HTTP_GET,
        .handler   = bq25895_css_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_css));

    httpd_uri_t bq25895_js = {
        .uri       = "/bq25895/js",
        .method    = HTTP_GET,
        .handler   = bq25895_js_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_js));

    // Register specific BQ25895 API endpoints
    httpd_uri_t bq25895_status_get = {
        .uri       = "/api/bq25895/status",
        .method    = HTTP_GET,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_status_get));

    httpd_uri_t bq25895_config_get = {
        .uri       = "/api/bq25895/config",
        .method    = HTTP_GET,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_config_get));

    httpd_uri_t bq25895_config_post = {
        .uri       = "/api/bq25895/config",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_config_post));

    httpd_uri_t bq25895_reset_post = {
        .uri       = "/api/bq25895/reset",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_reset_post));

    // Register CE pin control endpoint
    httpd_uri_t bq25895_ce_pin_post = {
        .uri       = "/api/bq25895/ce_pin",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_ce_pin_post));

    // Register register read/write endpoints
    httpd_uri_t bq25895_register_get = {
        .uri       = "/api/bq25895/register",
        .method    = HTTP_GET,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_register_get));

    httpd_uri_t bq25895_register_post = {
        .uri       = "/api/bq25895/register",
        .method    = HTTP_POST,
        .handler   = bq25895_api_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &bq25895_register_post));

    // Register pairing endpoint
    httpd_uri_t start_pairing_uri = {
        .uri = "/api/start_pairing",
        .method = HTTP_POST,
        .handler = start_pairing_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &start_pairing_uri));
    
    // Also register OPTIONS for CORS
    httpd_uri_t start_pairing_options = {
        .uri = "/api/start_pairing",
        .method = HTTP_OPTIONS,
        .handler = start_pairing_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &start_pairing_options));

    // Register cancel pairing endpoint
    httpd_uri_t cancel_pairing_uri = {
        .uri = "/api/cancel_pairing",
        .method = HTTP_POST,
        .handler = cancel_pairing_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &cancel_pairing_uri));
    
    // Also register OPTIONS for CORS
    httpd_uri_t cancel_pairing_options = {
        .uri = "/api/cancel_pairing",
        .method = HTTP_OPTIONS,
        .handler = cancel_pairing_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &cancel_pairing_options));

    // Register mDNS discovery endpoints
    httpd_uri_t discover_devices = {
        .uri = "/api/discover_devices",
        .method = HTTP_GET,
        .handler = discover_devices_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &discover_devices));
    
    // OPTIONS for CORS
    httpd_uri_t discover_devices_options = {
        .uri = "/api/discover_devices",
        .method = HTTP_OPTIONS,
        .handler = discover_devices_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &discover_devices_options));
    
    httpd_uri_t scream_devices = {
        .uri = "/api/scream_devices",
        .method = HTTP_GET,
        .handler = scream_devices_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &scream_devices));
    
    // OPTIONS for CORS
    httpd_uri_t scream_devices_options = {
        .uri = "/api/scream_devices",
        .method = HTTP_OPTIONS,
        .handler = scream_devices_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &scream_devices_options));
    
    // Register OTA endpoints
    httpd_uri_t ota_upload = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_upload));
    
    // OTA upload OPTIONS for CORS
    httpd_uri_t ota_upload_options = {
        .uri = "/api/ota/upload",
        .method = HTTP_OPTIONS,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_upload_options));
    
    httpd_uri_t ota_status = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_status));
    
    // OTA status OPTIONS for CORS
    httpd_uri_t ota_status_options = {
        .uri = "/api/ota/status",
        .method = HTTP_OPTIONS,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_status_options));
    
    httpd_uri_t ota_version = {
        .uri = "/api/ota/version",
        .method = HTTP_GET,
        .handler = ota_version_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_version));
    
    // OTA version OPTIONS for CORS
    httpd_uri_t ota_version_options = {
        .uri = "/api/ota/version",
        .method = HTTP_OPTIONS,
        .handler = ota_version_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_version_options));
    
    httpd_uri_t ota_rollback = {
        .uri = "/api/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_rollback));
    
    // OTA rollback OPTIONS for CORS
    httpd_uri_t ota_rollback_options = {
        .uri = "/api/ota/rollback",
        .method = HTTP_OPTIONS,
        .handler = ota_rollback_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &ota_rollback_options));

    // Register logs endpoint
    httpd_uri_t logs_get = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = logs_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &logs_get));

    // Register captive portal handlers
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &apple_cna));

    // Register catch-all handler (must be registered last)
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &redirect));

    // Only start DNS server for captive portal if in AP mode
    if (wifi_manager_get_state() == WIFI_MANAGER_STATE_AP_MODE) {
        start_dns_server();
    }

    ESP_LOGI(TAG, "Web server started successfully");
    
    // mDNS discovery is now started by mdns_service, no need to start it here

    return ESP_OK;
}

/**
 * Stop the web server
 */
esp_err_t web_server_stop(void)
{
    ESP_LOGI(TAG, "Stopping web server");
    
    // mDNS discovery is now managed by mdns_service, no need to stop it here

    // Stop DNS server
    stop_dns_server();

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
