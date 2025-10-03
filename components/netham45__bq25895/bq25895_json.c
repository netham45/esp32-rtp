#include "bq25895_json.h"
#include <stdbool.h>

cJSON *bq25895_status_to_json(const bq25895_status_t *status)
{
    if (!status) {
        return NULL;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddNumberToObject(json, "bat_voltage", status->bat_voltage);
    cJSON_AddNumberToObject(json, "sys_voltage", status->sys_voltage);
    cJSON_AddNumberToObject(json, "vbus_voltage", status->vbus_voltage);
    cJSON_AddNumberToObject(json, "charge_current", status->charge_current);
    cJSON_AddNumberToObject(json, "ts_voltage", status->ts_voltage);

    cJSON_AddNumberToObject(json, "vbus_stat", status->vbus_stat);
    cJSON_AddNumberToObject(json, "chg_stat", status->chg_stat);
    cJSON_AddBoolToObject(json, "pg_stat", status->pg_stat);
    cJSON_AddBoolToObject(json, "sdp_stat", status->sdp_stat);
    cJSON_AddBoolToObject(json, "vsys_stat", status->vsys_stat);

    cJSON_AddBoolToObject(json, "watchdog_fault", status->watchdog_fault);
    cJSON_AddBoolToObject(json, "boost_fault", status->boost_fault);
    cJSON_AddNumberToObject(json, "chg_fault", status->chg_fault);
    cJSON_AddBoolToObject(json, "bat_fault", status->bat_fault);
    cJSON_AddNumberToObject(json, "ntc_fault", status->ntc_fault);

    cJSON_AddBoolToObject(json, "therm_stat", status->therm_stat);

    return json;
}

cJSON *bq25895_params_to_json(const bq25895_charge_params_t *params)
{
    if (!params) {
        return NULL;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    cJSON_AddBoolToObject(json, "success", true);

    cJSON_AddNumberToObject(json, "charge_voltage_mv", params->charge_voltage_mv);
    cJSON_AddNumberToObject(json, "charge_current_ma", params->charge_current_ma);
    cJSON_AddNumberToObject(json, "input_current_limit_ma", params->input_current_limit_ma);
    cJSON_AddNumberToObject(json, "input_voltage_limit_mv", params->input_voltage_limit_mv);
    cJSON_AddNumberToObject(json, "precharge_current_ma", params->precharge_current_ma);
    cJSON_AddNumberToObject(json, "termination_current_ma", params->termination_current_ma);
    cJSON_AddNumberToObject(json, "boost_voltage_mv", params->boost_voltage_mv);

    cJSON_AddNumberToObject(json, "thermal_regulation_threshold", params->thermal_regulation_threshold);
    cJSON_AddNumberToObject(json, "fast_charge_timer_hours", params->fast_charge_timer_hours);

    cJSON_AddBoolToObject(json, "enable_charging", params->enable_charging);
    cJSON_AddBoolToObject(json, "enable_otg", params->enable_otg);
    cJSON_AddBoolToObject(json, "enable_termination", params->enable_termination);
    cJSON_AddBoolToObject(json, "enable_safety_timer", params->enable_safety_timer);
    cJSON_AddBoolToObject(json, "enable_hi_impedance", params->enable_hi_impedance);
    cJSON_AddBoolToObject(json, "enable_ir_compensation", params->enable_ir_compensation);

    cJSON_AddNumberToObject(json, "ir_compensation_mohm", params->ir_compensation_mohm);
    cJSON_AddNumberToObject(json, "ir_compensation_voltage_mv", params->ir_compensation_voltage_mv);

    return json;
}

esp_err_t bq25895_params_update_from_json(bq25895_charge_params_t *params, const cJSON *json)
{
    if (!params || !json) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *item;

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "charge_voltage_mv")) && cJSON_IsNumber(item)) {
        params->charge_voltage_mv = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "charge_current_ma")) && cJSON_IsNumber(item)) {
        params->charge_current_ma = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "input_current_limit_ma")) && cJSON_IsNumber(item)) {
        params->input_current_limit_ma = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "input_voltage_limit_mv")) && cJSON_IsNumber(item)) {
        params->input_voltage_limit_mv = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "precharge_current_ma")) && cJSON_IsNumber(item)) {
        params->precharge_current_ma = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "termination_current_ma")) && cJSON_IsNumber(item)) {
        params->termination_current_ma = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "boost_voltage_mv")) && cJSON_IsNumber(item)) {
        params->boost_voltage_mv = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "thermal_regulation_threshold")) && cJSON_IsNumber(item)) {
        params->thermal_regulation_threshold = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "fast_charge_timer_hours")) && cJSON_IsNumber(item)) {
        params->fast_charge_timer_hours = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "enable_charging")) && cJSON_IsBool(item)) {
        params->enable_charging = cJSON_IsTrue(item);
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "enable_otg")) && cJSON_IsBool(item)) {
        params->enable_otg = cJSON_IsTrue(item);
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "enable_termination")) && cJSON_IsBool(item)) {
        params->enable_termination = cJSON_IsTrue(item);
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "enable_safety_timer")) && cJSON_IsBool(item)) {
        params->enable_safety_timer = cJSON_IsTrue(item);
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "enable_hi_impedance")) && cJSON_IsBool(item)) {
        params->enable_hi_impedance = cJSON_IsTrue(item);
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "enable_ir_compensation")) && cJSON_IsBool(item)) {
        params->enable_ir_compensation = cJSON_IsTrue(item);
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "ir_compensation_mohm")) && cJSON_IsNumber(item)) {
        params->ir_compensation_mohm = item->valueint;
    }

    if ((item = cJSON_GetObjectItemCaseSensitive(json, "ir_compensation_voltage_mv")) && cJSON_IsNumber(item)) {
        params->ir_compensation_voltage_mv = item->valueint;
    }

    return ESP_OK;
}