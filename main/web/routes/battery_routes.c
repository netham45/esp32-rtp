#include "battery_routes.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "lifecycle_manager.h"
#include "bq25895_json.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "battery_routes";

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json)
{
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build JSON");
        return ESP_FAIL;
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_send(req, payload, strlen(payload));
    free(payload);
    return rc;
}

static esp_err_t send_error_response(httpd_req_t *req, const char *status, const char *message)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "message", message ? message : "Unknown error");

    return send_json_response(req, json);
}

static esp_err_t read_request_body(httpd_req_t *req, char **out)
{
    size_t total_len = req->content_len;
    if (total_len == 0) {
        *out = NULL;
        return ESP_OK;
    }

    char *buffer = malloc(total_len + 1);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buffer + received, total_len - received);
        if (ret <= 0) {
            free(buffer);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        received += ret;
    }

    buffer[total_len] = '\0';
    *out = buffer;
    return ESP_OK;
}

static esp_err_t handle_status_get(httpd_req_t *req)
{
    bq25895_status_t status;
    esp_err_t rc = lifecycle_get_battery_status(&status);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get battery status: 0x%x", rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to get battery status");
    }

    cJSON *json = bq25895_status_to_json(&status);
    return send_json_response(req, json);
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    bq25895_charge_params_t params;
    esp_err_t rc = lifecycle_get_battery_params(&params);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get battery params: 0x%x", rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to get battery configuration");
    }

    cJSON *json = bq25895_params_to_json(&params);
    return send_json_response(req, json);
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t rc = read_request_body(req, &body);
    if (rc != ESP_OK) {
        return send_error_response(req, "500 Internal Server Error", "Failed to read request body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        return send_error_response(req, "400 Bad Request", "Invalid JSON payload");
    }

    bq25895_charge_params_t params;
    rc = lifecycle_get_battery_params(&params);
    if (rc != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to fetch current params: 0x%x", rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to fetch current configuration");
    }

    rc = bq25895_params_update_from_json(&params, root);
    cJSON_Delete(root);
    if (rc != ESP_OK) {
        return send_error_response(req, "400 Bad Request", "Invalid configuration values");
    }

    rc = lifecycle_set_battery_params(&params);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply params: 0x%x", rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to apply configuration");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    return send_json_response(req, json);
}

static esp_err_t handle_reset_post(httpd_req_t *req)
{
    esp_err_t rc = lifecycle_reset_battery();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset charger: 0x%x", rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to reset device");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    return send_json_response(req, json);
}

static esp_err_t handle_ce_pin_post(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t rc = read_request_body(req, &body);
    if (rc != ESP_OK) {
        return send_error_response(req, "500 Internal Server Error", "Failed to read request body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        return send_error_response(req, "400 Bad Request", "Invalid JSON payload");
    }

    const cJSON *enable_item = cJSON_GetObjectItemCaseSensitive(root, "enable");
    if (!cJSON_IsBool(enable_item)) {
        cJSON_Delete(root);
        return send_error_response(req, "400 Bad Request", "Missing or invalid 'enable' flag");
    }

    bool enable = cJSON_IsTrue(enable_item);
    rc = lifecycle_set_battery_ce_pin(enable);
    cJSON_Delete(root);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CE pin: 0x%x", rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to set CE pin");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    return send_json_response(req, json);
}

static esp_err_t parse_register_query(httpd_req_t *req, uint8_t *address)
{
    int query_len = httpd_req_get_url_query_len(req);
    if (query_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *query = malloc(query_len + 1);
    if (!query) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t rc = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (rc != ESP_OK) {
        free(query);
        return rc;
    }

    char value_buf[16];
    rc = httpd_query_key_value(query, "address", value_buf, sizeof(value_buf));
    free(query);

    if (rc != ESP_OK) {
        return rc;
    }

    char *end = NULL;
    long addr = strtol(value_buf, &end, 0);
    if (end == value_buf || *end != '\0' || addr < 0 || addr > 0xFF) {
        return ESP_ERR_INVALID_ARG;
    }

    *address = (uint8_t)addr;
    return ESP_OK;
}

static esp_err_t handle_register_get(httpd_req_t *req)
{
    uint8_t address = 0;
    esp_err_t rc = parse_register_query(req, &address);
    if (rc == ESP_ERR_INVALID_ARG) {
        return send_error_response(req, "400 Bad Request", "Missing or invalid address parameter");
    } else if (rc != ESP_OK) {
        return send_error_response(req, "500 Internal Server Error", "Failed to parse query");
    }

    uint8_t value = 0;
    rc = lifecycle_read_battery_register(address, &value);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register 0x%02X: 0x%x", address, rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to read register");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddNumberToObject(json, "address", address);
    cJSON_AddNumberToObject(json, "value", value);
    return send_json_response(req, json);
}

static esp_err_t handle_register_post(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t rc = read_request_body(req, &body);
    if (rc != ESP_OK) {
        return send_error_response(req, "500 Internal Server Error", "Failed to read request body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        return send_error_response(req, "400 Bad Request", "Invalid JSON payload");
    }

    const cJSON *addr_item = cJSON_GetObjectItemCaseSensitive(root, "address");
    const cJSON *value_item = cJSON_GetObjectItemCaseSensitive(root, "value");

    if (!cJSON_IsNumber(addr_item) || !cJSON_IsNumber(value_item)) {
        cJSON_Delete(root);
        return send_error_response(req, "400 Bad Request", "Missing or invalid address/value");
    }

    int addr = addr_item->valueint;
    int value = value_item->valueint;
    cJSON_Delete(root);

    if (addr < 0 || addr > 0xFF || value < 0 || value > 0xFF) {
        return send_error_response(req, "400 Bad Request", "Address/value out of range");
    }

    rc = lifecycle_write_battery_register((uint8_t)addr, (uint8_t)value);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register 0x%02X: 0x%x", addr, rc);
        return send_error_response(req, "500 Internal Server Error", "Failed to write register");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddNumberToObject(json, "address", addr);
    cJSON_AddNumberToObject(json, "value", value);
    return send_json_response(req, json);
}

esp_err_t register_battery_routes(httpd_handle_t server)
{
    esp_err_t ret;

    httpd_uri_t status_get = {
        .uri = "/api/bq25895/status",
        .method = HTTP_GET,
        .handler = handle_status_get,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &status_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status GET: 0x%x", ret);
        return ret;
    }

    httpd_uri_t config_get = {
        .uri = "/api/bq25895/config",
        .method = HTTP_GET,
        .handler = handle_config_get,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &config_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config GET: 0x%x", ret);
        return ret;
    }

    httpd_uri_t config_post = {
        .uri = "/api/bq25895/config",
        .method = HTTP_POST,
        .handler = handle_config_post,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &config_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config POST: 0x%x", ret);
        return ret;
    }

    httpd_uri_t reset_post = {
        .uri = "/api/bq25895/reset",
        .method = HTTP_POST,
        .handler = handle_reset_post,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &reset_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register reset POST: 0x%x", ret);
        return ret;
    }

    httpd_uri_t ce_pin_post = {
        .uri = "/api/bq25895/ce_pin",
        .method = HTTP_POST,
        .handler = handle_ce_pin_post,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &ce_pin_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ce_pin POST: 0x%x", ret);
        return ret;
    }

    httpd_uri_t reg_get = {
        .uri = "/api/bq25895/register",
        .method = HTTP_GET,
        .handler = handle_register_get,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &reg_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register register GET: 0x%x", ret);
        return ret;
    }

    httpd_uri_t reg_post = {
        .uri = "/api/bq25895/register",
        .method = HTTP_POST,
        .handler = handle_register_post,
        .user_ctx = NULL
    };
    ret = httpd_register_uri_handler(server, &reg_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register register POST: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Battery routes registered successfully");
    return ESP_OK;
}