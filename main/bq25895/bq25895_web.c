/**
 * @file bq25895_web.c
 * @brief Web interface for BQ25895 battery charger IC
 */

#include "bq25895_web.h"
#include "bq25895.h"
#include "bq25895_integration.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "bq25895_web";

// Forward declarations
static esp_err_t bq25895_get_status_json(char **json_str);
static esp_err_t bq25895_get_params_json(char **json_str);
static esp_err_t bq25895_set_params_from_json(const char *json_str);

/**
 * @brief Initialize the BQ25895 web interface
 */
esp_err_t bq25895_web_init(void)
{
    ESP_LOGI(TAG, "Initializing BQ25895 web interface");
    return ESP_OK;
}

/**
 * @brief Get the HTML content for the BQ25895 web interface
 */
const char *bq25895_web_get_html(void)
{
    static const char html[] = 
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>BQ25895 Battery Charger Configuration</title>\n"
        "    <link rel=\"stylesheet\" href=\"/bq25895/css\">\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <h1>BQ25895 Battery Charger Configuration</h1>\n"
        "        \n"
        "        <div class=\"status-container\">\n"
        "            <h2>Status</h2>\n"
        "            <div class=\"status-grid\">\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Battery Voltage:</span>\n"
        "                    <span id=\"bat-voltage\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">System Voltage:</span>\n"
        "                    <span id=\"sys-voltage\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">VBUS Voltage:</span>\n"
        "                    <span id=\"vbus-voltage\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Charge Current:</span>\n"
        "                    <span id=\"charge-current\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Charging Status:</span>\n"
        "                    <span id=\"charging-status\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">VBUS Status:</span>\n"
        "                    <span id=\"vbus-status\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Power Good:</span>\n"
        "                    <span id=\"power-good\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Thermal Status:</span>\n"
        "                    <span id=\"thermal-status\" class=\"value\">--</span>\n"
        "                </div>\n"
        "            </div>\n"
        "            <div class=\"status-grid\">\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Watchdog Fault:</span>\n"
        "                    <span id=\"watchdog-fault\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Boost Fault:</span>\n"
        "                    <span id=\"boost-fault\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Charge Fault:</span>\n"
        "                    <span id=\"charge-fault\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">Battery Fault:</span>\n"
        "                    <span id=\"battery-fault\" class=\"value\">--</span>\n"
        "                </div>\n"
        "                <div class=\"status-item\">\n"
        "                    <span class=\"label\">NTC Fault:</span>\n"
        "                    <span id=\"ntc-fault\" class=\"value\">--</span>\n"
        "                </div>\n"
        "            </div>\n"
        "            <button id=\"refresh-status\" class=\"btn\">Refresh Status</button>\n"
        "            <div class=\"ce-pin-control\">\n"
        "                <h3>CE Pin Control (IO12)</h3>\n"
        "                <p>The CE pin is active low. When low, charging is enabled. When high, charging is disabled.</p>\n"
        "                <div class=\"button-group\">\n"
        "                    <button type=\"button\" id=\"ce-pin-enable\" class=\"btn\">Enable Charging (CE=Low)</button>\n"
        "                    <button type=\"button\" id=\"ce-pin-disable\" class=\"btn\">Disable Charging (CE=High)</button>\n"
        "                </div>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <div class=\"config-container\">\n"
        "            <h2>Configuration</h2>\n"
        "            <form id=\"config-form\">\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"charge-voltage\">Charge Voltage (mV):</label>\n"
        "                    <input type=\"number\" id=\"charge-voltage\" min=\"3840\" max=\"4608\" step=\"16\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"charge-current\">Charge Current (mA):</label>\n"
        "                    <input type=\"number\" id=\"charge-current-input\" min=\"0\" max=\"5056\" step=\"64\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"input-current-limit\">Input Current Limit (mA):</label>\n"
        "                    <input type=\"number\" id=\"input-current-limit\" min=\"100\" max=\"3250\" step=\"50\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"input-voltage-limit\">Input Voltage Limit (mV):</label>\n"
        "                    <input type=\"number\" id=\"input-voltage-limit\" min=\"3900\" max=\"14000\" step=\"100\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"precharge-current\">Precharge Current (mA):</label>\n"
        "                    <input type=\"number\" id=\"precharge-current\" min=\"64\" max=\"1024\" step=\"64\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"termination-current\">Termination Current (mA):</label>\n"
        "                    <input type=\"number\" id=\"termination-current\" min=\"64\" max=\"1024\" step=\"64\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"boost-voltage\">Boost Voltage (mV):</label>\n"
        "                    <input type=\"number\" id=\"boost-voltage\" min=\"4550\" max=\"5510\" step=\"64\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"thermal-regulation\">Thermal Regulation (°C):</label>\n"
        "                    <select id=\"thermal-regulation\">\n"
        "                        <option value=\"60\">60°C</option>\n"
        "                        <option value=\"80\">80°C</option>\n"
        "                        <option value=\"100\">100°C</option>\n"
        "                        <option value=\"120\">120°C</option>\n"
        "                    </select>\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"fast-charge-timer\">Fast Charge Timer (hours):</label>\n"
        "                    <select id=\"fast-charge-timer\">\n"
        "                        <option value=\"5\">5 hours</option>\n"
        "                        <option value=\"8\">8 hours</option>\n"
        "                        <option value=\"12\">12 hours</option>\n"
        "                        <option value=\"20\">20 hours</option>\n"
        "                    </select>\n"
        "                </div>\n"
        "                <div class=\"form-group checkbox\">\n"
        "                    <input type=\"checkbox\" id=\"enable-charging\">\n"
        "                    <label for=\"enable-charging\">Enable Charging</label>\n"
        "                </div>\n"
        "                <div class=\"form-group checkbox\">\n"
        "                    <input type=\"checkbox\" id=\"enable-otg\">\n"
        "                    <label for=\"enable-otg\">Enable OTG Mode</label>\n"
        "                </div>\n"
        "                <div class=\"form-group checkbox\">\n"
        "                    <input type=\"checkbox\" id=\"enable-termination\">\n"
        "                    <label for=\"enable-termination\">Enable Termination</label>\n"
        "                </div>\n"
        "                <div class=\"form-group checkbox\">\n"
        "                    <input type=\"checkbox\" id=\"enable-safety-timer\">\n"
        "                    <label for=\"enable-safety-timer\">Enable Safety Timer</label>\n"
        "                </div>\n"
        "                <div class=\"form-group checkbox\">\n"
        "                    <input type=\"checkbox\" id=\"enable-hi-impedance\">\n"
        "                    <label for=\"enable-hi-impedance\">Enable Hi-Z Mode</label>\n"
        "                </div>\n"
        "                <div class=\"form-group checkbox\">\n"
        "                    <input type=\"checkbox\" id=\"enable-ir-compensation\">\n"
        "                    <label for=\"enable-ir-compensation\">Enable IR Compensation</label>\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"ir-compensation-mohm\">IR Compensation (mOhm):</label>\n"
        "                    <input type=\"number\" id=\"ir-compensation-mohm\" min=\"0\" max=\"140\" step=\"20\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"ir-compensation-voltage\">IR Compensation Voltage (mV):</label>\n"
        "                    <input type=\"number\" id=\"ir-compensation-voltage\" min=\"0\" max=\"224\" step=\"32\">\n"
        "                </div>\n"
        "                <div class=\"button-group\">\n"
        "                    <button type=\"button\" id=\"load-config\" class=\"btn\">Load Configuration</button>\n"
        "                    <button type=\"submit\" class=\"btn primary\">Save Configuration</button>\n"
        "                    <button type=\"button\" id=\"reset-device\" class=\"btn danger\">Reset Device</button>\n"
        "                </div>\n"
        "            </form>\n"
        "        </div>\n"
        "\n"
        "        <div class=\"register-container\">\n"
        "            <h2>Register Access</h2>\n"
        "            <p>Read and write arbitrary registers for advanced configuration and debugging.</p>\n"
        "            <div class=\"register-controls\">\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"register-address\">Register Address (hex):</label>\n"
        "                    <input type=\"text\" id=\"register-address\" placeholder=\"0x00\" maxlength=\"4\">\n"
        "                </div>\n"
        "                <div class=\"form-group\">\n"
        "                    <label for=\"register-value\">Register Value (hex):</label>\n"
        "                    <input type=\"text\" id=\"register-value\" placeholder=\"0x00\" maxlength=\"4\">\n"
        "                </div>\n"
        "                <div class=\"button-group\">\n"
        "                    <button type=\"button\" id=\"read-register\" class=\"btn\">Read Register</button>\n"
        "                    <button type=\"button\" id=\"write-register\" class=\"btn primary\">Write Register</button>\n"
        "                </div>\n"
        "            </div>\n"
        "            <div class=\"register-result\">\n"
        "                <h3>Result:</h3>\n"
        "                <pre id=\"register-result\">No operation performed yet.</pre>\n"
        "            </div>\n"
        "        </div>\n"
        "    </div>\n"
        "    <script src=\"/bq25895/js\"></script>\n"
        "</body>\n"
        "</html>\n";

    return html;
}

/**
 * @brief Get the JavaScript content for the BQ25895 web interface
 */
const char *bq25895_web_get_js(void)
{
    static const char js[] = 
        "document.addEventListener('DOMContentLoaded', function() {\n"
        "    // Elements\n"
        "    const refreshStatusBtn = document.getElementById('refresh-status');\n"
        "    const loadConfigBtn = document.getElementById('load-config');\n"
        "    const resetDeviceBtn = document.getElementById('reset-device');\n"
        "    const configForm = document.getElementById('config-form');\n"
        "    const cePinEnableBtn = document.getElementById('ce-pin-enable');\n"
        "    const cePinDisableBtn = document.getElementById('ce-pin-disable');\n"
        "\n"
        "    // Load initial status\n"
        "    loadStatus();\n"
        "\n"
        "    // Event listeners\n"
        "    refreshStatusBtn.addEventListener('click', loadStatus);\n"
        "    loadConfigBtn.addEventListener('click', loadConfig);\n"
        "    resetDeviceBtn.addEventListener('click', resetDevice);\n"
        "    cePinEnableBtn.addEventListener('click', function() {\n"
        "        setCePin(true);\n"
        "    });\n"
        "    cePinDisableBtn.addEventListener('click', function() {\n"
        "        setCePin(false);\n"
        "    });\n"
        "    configForm.addEventListener('submit', function(e) {\n"
        "        e.preventDefault();\n"
        "        saveConfig();\n"
        "    });\n"
        "\n"
        "    // Functions\n"
        "    function loadStatus() {\n"
        "        fetch('/api/bq25895/status')\n"
        "            .then(response => response.json())\n"
        "            .then(data => {\n"
        "                // Check if the request was successful\n"
        "                if (!data.success) {\n"
        "                    console.error('Error loading status:', data.message);\n"
        "                    alert('Failed to load status: ' + data.message);\n"
        "                    return;\n"
        "                }\n"
        "                \n"
        "                // Update status values\n"
        "                document.getElementById('bat-voltage').textContent = data.bat_voltage.toFixed(2) + ' V';\n"
        "                document.getElementById('sys-voltage').textContent = data.sys_voltage.toFixed(2) + ' V';\n"
        "                document.getElementById('vbus-voltage').textContent = data.vbus_voltage.toFixed(2) + ' V';\n"
        "                document.getElementById('charge-current').textContent = data.charge_current.toFixed(2) + ' A';\n"
        "\n"
        "                // Charging status\n"
        "                let chgStatus = 'Unknown';\n"
        "                switch(data.chg_stat) {\n"
        "                    case 0: chgStatus = 'Not Charging'; break;\n"
        "                    case 1: chgStatus = 'Pre-charge'; break;\n"
        "                    case 2: chgStatus = 'Fast Charging'; break;\n"
        "                    case 3: chgStatus = 'Charge Done'; break;\n"
        "                }\n"
        "                document.getElementById('charging-status').textContent = chgStatus;\n"
        "\n"
        "                // VBUS status\n"
        "                let vbusStatus = 'Unknown';\n"
        "                switch(data.vbus_stat) {\n"
        "                    case 0: vbusStatus = 'No Input'; break;\n"
        "                    case 1: vbusStatus = 'USB Host SDP'; break;\n"
        "                    case 2: vbusStatus = 'USB CDP'; break;\n"
        "                    case 3: vbusStatus = 'USB DCP'; break;\n"
        "                    case 4: vbusStatus = 'MaxCharge'; break;\n"
        "                    case 5: vbusStatus = 'Unknown Adapter'; break;\n"
        "                    case 6: vbusStatus = 'Non-Standard Adapter'; break;\n"
        "                    case 7: vbusStatus = 'OTG'; break;\n"
        "                }\n"
        "                document.getElementById('vbus-status').textContent = vbusStatus;\n"
        "\n"
        "                // Power good\n"
        "                document.getElementById('power-good').textContent = data.pg_stat ? 'Yes' : 'No';\n"
        "\n"
        "                // Thermal status\n"
        "                document.getElementById('thermal-status').textContent = data.therm_stat ? 'In Regulation' : 'Normal';\n"
        "\n"
        "                // Fault status\n"
        "                document.getElementById('watchdog-fault').textContent = data.watchdog_fault ? 'Yes' : 'No';\n"
        "                document.getElementById('boost-fault').textContent = data.boost_fault ? 'Yes' : 'No';\n"
        "\n"
        "                // Charge fault\n"
        "                let chgFault = 'Normal';\n"
        "                switch(data.chg_fault) {\n"
        "                    case 0: chgFault = 'Normal'; break;\n"
        "                    case 1: chgFault = 'Input Fault'; break;\n"
        "                    case 2: chgFault = 'Thermal Shutdown'; break;\n"
        "                    case 3: chgFault = 'Timer Expired'; break;\n"
        "                }\n"
        "                document.getElementById('charge-fault').textContent = chgFault;\n"
        "\n"
        "                document.getElementById('battery-fault').textContent = data.bat_fault ? 'Yes' : 'No';\n"
        "\n"
        "                // NTC fault\n"
        "                let ntcFault = 'Normal';\n"
        "                switch(data.ntc_fault) {\n"
        "                    case 0: ntcFault = 'Normal'; break;\n"
        "                    case 1: ntcFault = 'Cold'; break;\n"
        "                    case 2: ntcFault = 'Hot'; break;\n"
        "                }\n"
        "                document.getElementById('ntc-fault').textContent = ntcFault;\n"
        "            })\n"
        "            .catch(error => {\n"
        "                console.error('Error loading status:', error);\n"
        "                alert('Failed to load status. Please try again.');\n"
        "            });\n"
        "    }\n"
        "\n"
        "    function loadConfig() {\n"
        "        fetch('/api/bq25895/config')\n"
        "            .then(response => response.json())\n"
        "            .then(data => {\n"
        "                // Check if the request was successful\n"
        "                if (!data.success) {\n"
        "                    console.error('Error loading configuration:', data.message);\n"
        "                    alert('Failed to load configuration: ' + data.message);\n"
        "                    return;\n"
        "                }\n"
        "                \n"
        "                // Update form values\n"
        "                document.getElementById('charge-voltage').value = data.charge_voltage_mv;\n"
        "                document.getElementById('charge-current-input').value = data.charge_current_ma;\n"
        "                document.getElementById('input-current-limit').value = data.input_current_limit_ma;\n"
        "                document.getElementById('input-voltage-limit').value = data.input_voltage_limit_mv;\n"
        "                document.getElementById('precharge-current').value = data.precharge_current_ma;\n"
        "                document.getElementById('termination-current').value = data.termination_current_ma;\n"
        "                document.getElementById('boost-voltage').value = data.boost_voltage_mv;\n"
        "                document.getElementById('thermal-regulation').value = data.thermal_regulation_threshold;\n"
        "                document.getElementById('fast-charge-timer').value = data.fast_charge_timer_hours;\n"
        "                document.getElementById('enable-charging').checked = data.enable_charging;\n"
        "                document.getElementById('enable-otg').checked = data.enable_otg;\n"
        "                document.getElementById('enable-termination').checked = data.enable_termination;\n"
        "                document.getElementById('enable-safety-timer').checked = data.enable_safety_timer;\n"
        "                document.getElementById('enable-hi-impedance').checked = data.enable_hi_impedance;\n"
        "                document.getElementById('enable-ir-compensation').checked = data.enable_ir_compensation;\n"
        "                document.getElementById('ir-compensation-mohm').value = data.ir_compensation_mohm;\n"
        "                document.getElementById('ir-compensation-voltage').value = data.ir_compensation_voltage_mv;\n"
        "            })\n"
        "            .catch(error => {\n"
        "                console.error('Error loading configuration:', error);\n"
        "                alert('Failed to load configuration. Please try again.');\n"
        "            });\n"
        "    }\n"
        "\n"
        "    function saveConfig() {\n"
        "        const config = {\n"
        "            charge_voltage_mv: parseInt(document.getElementById('charge-voltage').value),\n"
        "            charge_current_ma: parseInt(document.getElementById('charge-current-input').value),\n"
        "            input_current_limit_ma: parseInt(document.getElementById('input-current-limit').value),\n"
        "            input_voltage_limit_mv: parseInt(document.getElementById('input-voltage-limit').value),\n"
        "            precharge_current_ma: parseInt(document.getElementById('precharge-current').value),\n"
        "            termination_current_ma: parseInt(document.getElementById('termination-current').value),\n"
        "            boost_voltage_mv: parseInt(document.getElementById('boost-voltage').value),\n"
        "            thermal_regulation_threshold: parseInt(document.getElementById('thermal-regulation').value),\n"
        "            fast_charge_timer_hours: parseInt(document.getElementById('fast-charge-timer').value),\n"
        "            enable_charging: document.getElementById('enable-charging').checked,\n"
        "            enable_otg: document.getElementById('enable-otg').checked,\n"
        "            enable_termination: document.getElementById('enable-termination').checked,\n"
        "            enable_safety_timer: document.getElementById('enable-safety-timer').checked,\n"
        "            enable_hi_impedance: document.getElementById('enable-hi-impedance').checked,\n"
        "            enable_ir_compensation: document.getElementById('enable-ir-compensation').checked,\n"
        "            ir_compensation_mohm: parseInt(document.getElementById('ir-compensation-mohm').value),\n"
        "            ir_compensation_voltage_mv: parseInt(document.getElementById('ir-compensation-voltage').value)\n"
        "        };\n"
        "\n"
        "        fetch('/api/bq25895/config', {\n"
        "            method: 'POST',\n"
        "            headers: {\n"
        "                'Content-Type': 'application/json'\n"
        "            },\n"
        "            body: JSON.stringify(config)\n"
        "        })\n"
        "        .then(response => response.json())\n"
        "        .then(data => {\n"
        "            if (data.success) {\n"
        "                alert('Configuration saved successfully!');\n"
        "                loadStatus();\n"
        "            } else {\n"
        "                alert('Failed to save configuration: ' + data.message);\n"
        "            }\n"
        "        })\n"
        "        .catch(error => {\n"
        "            console.error('Error saving configuration:', error);\n"
        "            alert('Failed to save configuration. Please try again.');\n"
        "        });\n"
        "    }\n"
        "\n"
        "    function resetDevice() {\n"
        "        if (confirm('Are you sure you want to reset the BQ25895 device?')) {\n"
        "            fetch('/api/bq25895/reset', {\n"
        "                method: 'POST'\n"
        "            })\n"
        "            .then(response => response.json())\n"
        "            .then(data => {\n"
        "                if (data.success) {\n"
        "                    alert('Device reset successfully!');\n"
        "                    loadStatus();\n"
        "                    loadConfig();\n"
        "                } else {\n"
        "                    alert('Failed to reset device: ' + data.message);\n"
        "                }\n"
        "            })\n"
        "            .catch(error => {\n"
        "                console.error('Error resetting device:', error);\n"
        "                alert('Failed to reset device. Please try again.');\n"
        "            });\n"
        "        }\n"
        "    }\n"
        "\n"
        "    function setCePin(enable) {\n"
        "        fetch('/api/bq25895/ce_pin', {\n"
        "            method: 'POST',\n"
        "            headers: {\n"
        "                'Content-Type': 'application/json'\n"
        "            },\n"
        "            body: JSON.stringify({ enable: enable })\n"
        "        })\n"
        "        .then(response => response.json())\n"
        "        .then(data => {\n"
        "            if (data.success) {\n"
        "                alert('CE pin ' + (enable ? 'enabled' : 'disabled') + ' successfully!');\n"
        "                loadStatus();\n"
        "            } else {\n"
        "                alert('Failed to set CE pin: ' + data.message);\n"
        "            }\n"
        "        })\n"
        "        .catch(error => {\n"
        "            console.error('Error setting CE pin:', error);\n"
        "            alert('Failed to set CE pin. Please try again.');\n"
        "        });\n"
        "    }\n"
        "\n"
        "    // Register read/write functions\n"
        "    const readRegisterBtn = document.getElementById('read-register');\n"
        "    const writeRegisterBtn = document.getElementById('write-register');\n"
        "    const registerAddressInput = document.getElementById('register-address');\n"
        "    const registerValueInput = document.getElementById('register-value');\n"
        "    const registerResult = document.getElementById('register-result');\n"
        "\n"
        "    readRegisterBtn.addEventListener('click', readRegister);\n"
        "    writeRegisterBtn.addEventListener('click', writeRegister);\n"
        "\n"
        "    function readRegister() {\n"
        "        const regAddress = parseHexInput(registerAddressInput.value);\n"
        "        if (regAddress === null) {\n"
        "            alert('Please enter a valid register address (0x00-0xFF)');\n"
        "            return;\n"
        "        }\n"
        "\n"
        "        fetch(`/api/bq25895/register?address=${regAddress}`)\n"
        "            .then(response => response.json())\n"
        "            .then(data => {\n"
        "                if (data.success) {\n"
        "                    registerResult.textContent = `Read register 0x${regAddress.toString(16).padStart(2, '0').toUpperCase()}: 0x${data.value.toString(16).padStart(2, '0').toUpperCase()}`;\n"
        "                    registerValueInput.value = '0x' + data.value.toString(16).padStart(2, '0').toUpperCase();\n"
        "                } else {\n"
        "                    registerResult.textContent = `Error: ${data.message}`;\n"
        "                }\n"
        "            })\n"
        "            .catch(error => {\n"
        "                console.error('Error reading register:', error);\n"
        "                registerResult.textContent = 'Error: Failed to read register. Please try again.';\n"
        "            });\n"
        "    }\n"
        "\n"
        "    function writeRegister() {\n"
        "        const regAddress = parseHexInput(registerAddressInput.value);\n"
        "        if (regAddress === null) {\n"
        "            alert('Please enter a valid register address (0x00-0xFF)');\n"
        "            return;\n"
        "        }\n"
        "\n"
        "        const regValue = parseHexInput(registerValueInput.value);\n"
        "        if (regValue === null) {\n"
        "            alert('Please enter a valid register value (0x00-0xFF)');\n"
        "            return;\n"
        "        }\n"
        "\n"
        "        fetch('/api/bq25895/register', {\n"
        "            method: 'POST',\n"
        "            headers: {\n"
        "                'Content-Type': 'application/json'\n"
        "            },\n"
        "            body: JSON.stringify({ address: regAddress, value: regValue })\n"
        "        })\n"
        "        .then(response => response.json())\n"
        "        .then(data => {\n"
        "            if (data.success) {\n"
        "                registerResult.textContent = `Wrote 0x${regValue.toString(16).padStart(2, '0').toUpperCase()} to register 0x${regAddress.toString(16).padStart(2, '0').toUpperCase()}`;\n"
        "            } else {\n"
        "                registerResult.textContent = `Error: ${data.message}`;\n"
        "            }\n"
        "        })\n"
        "        .catch(error => {\n"
        "            console.error('Error writing register:', error);\n"
        "            registerResult.textContent = 'Error: Failed to write register. Please try again.';\n"
        "        });\n"
        "    }\n"
        "\n"
        "    function parseHexInput(input) {\n"
        "        if (!input) return null;\n"
        "        \n"
        "        // Remove '0x' prefix if present\n"
        "        if (input.startsWith('0x') || input.startsWith('0X')) {\n"
        "            input = input.substring(2);\n"
        "        }\n"
        "        \n"
        "        // Parse as hex\n"
        "        const value = parseInt(input, 16);\n"
        "        \n"
        "        // Check if valid and in range\n"
        "        if (isNaN(value) || value < 0 || value > 255) {\n"
        "            return null;\n"
        "        }\n"
        "        \n"
        "        return value;\n"
        "    }\n"
        "});\n";

    return js;
}

/**
 * @brief Get the CSS content for the BQ25895 web interface
 */
const char *bq25895_web_get_css(void)
{
    static const char css[] = 
        "* {\n"
        "    box-sizing: border-box;\n"
        "    margin: 0;\n"
        "    padding: 0;\n"
        "}\n"
        "\n"
        "body {\n"
        "    font-family: Arial, sans-serif;\n"
        "    line-height: 1.6;\n"
        "    color: #333;\n"
        "    background-color: #f4f4f4;\n"
        "    padding: 20px;\n"
        "}\n"
        "\n"
        ".container {\n"
        "    max-width: 1200px;\n"
        "    margin: 0 auto;\n"
        "    background-color: #fff;\n"
        "    padding: 20px;\n"
        "    border-radius: 5px;\n"
        "    box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);\n"
        "}\n"
        "\n"
        "h1 {\n"
        "    text-align: center;\n"
        "    margin-bottom: 20px;\n"
        "    color: #333;\n"
        "}\n"
        "\n"
        "h2 {\n"
        "    margin-bottom: 15px;\n"
        "    color: #444;\n"
        "    border-bottom: 1px solid #ddd;\n"
        "    padding-bottom: 5px;\n"
        "}\n"
        "\n"
        ".status-container, .config-container {\n"
        "    margin-bottom: 30px;\n"
        "}\n"
        "\n"
        ".status-grid {\n"
        "    display: grid;\n"
        "    grid-template-columns: repeat(auto-fill, minmax(250px, 1fr));\n"
        "    gap: 15px;\n"
        "    margin-bottom: 20px;\n"
        "}\n"
        "\n"
        ".status-item {\n"
        "    background-color: #f9f9f9;\n"
        "    padding: 10px;\n"
        "    border-radius: 4px;\n"
        "    border: 1px solid #ddd;\n"
        "}\n"
        "\n"
        ".label {\n"
        "    font-weight: bold;\n"
        "    display: block;\n"
        "    margin-bottom: 5px;\n"
        "    color: #555;\n"
        "}\n"
        "\n"
        ".value {\n"
        "    font-size: 1.1em;\n"
        "    color: #333;\n"
        "}\n"
        "\n"
        ".form-group {\n"
        "    margin-bottom: 15px;\n"
        "}\n"
        "\n"
        ".form-group label {\n"
        "    display: block;\n"
        "    margin-bottom: 5px;\n"
        "    font-weight: bold;\n"
        "    color: #555;\n"
        "}\n"
        "\n"
        ".form-group input[type=\"number\"],\n"
        ".form-group select {\n"
        "    width: 100%;\n"
        "    padding: 8px;\n"
        "    border: 1px solid #ddd;\n"
        "    border-radius: 4px;\n"
        "    font-size: 16px;\n"
        "}\n"
        "\n"
        ".form-group.checkbox {\n"
        "    display: flex;\n"
        "    align-items: center;\n"
        "}\n"
        "\n"
        ".form-group.checkbox input {\n"
        "    margin-right: 10px;\n"
        "    transform: scale(1.2);\n"
        "}\n"
        "\n"
        ".form-group.checkbox label {\n"
        "    margin-bottom: 0;\n"
        "}\n"
        "\n"
        ".button-group {\n"
        "    display: flex;\n"
        "    justify-content: space-between;\n"
        "    margin-top: 20px;\n"
        "}\n"
        "\n"
        ".btn {\n"
        "    padding: 10px 15px;\n"
        "    border: none;\n"
        "    border-radius: 4px;\n"
        "    cursor: pointer;\n"
        "    font-size: 16px;\n"
        "    background-color: #f0f0f0;\n"
        "    color: #333;\n"
        "    transition: background-color 0.3s;\n"
        "}\n"
        "\n"
        ".btn:hover {\n"
        "    background-color: #e0e0e0;\n"
        "}\n"
        "\n"
        ".btn.primary {\n"
        "    background-color: #4CAF50;\n"
        "    color: white;\n"
        "}\n"
        "\n"
        ".btn.primary:hover {\n"
        "        grid-template-columns: 1fr;\n"
        "    }\n"
        "    \n"
        "    .button-group {\n"
        "        flex-direction: column;\n"
        "        gap: 10px;\n"
        "    }\n"
        "    \n"
        "    .btn {\n"
        "        width: 100%;\n"
        "    }\n"
        "}\n";

    return css;
}

/**
 * @brief Handle HTTP requests for the BQ25895 web interface
 */
esp_err_t bq25895_web_handle_request(const char *uri, const char *method, const char *content, size_t content_len, char **response, size_t *response_len)
{
    ESP_LOGI(TAG, "Handling request: %s %s", method, uri);

    if (strcmp(uri, "/api/bq25895/status") == 0 && strcmp(method, "GET") == 0) {
        // Get BQ25895 status
        esp_err_t ret = bq25895_get_status_json(response);
        if (ret == ESP_OK && *response != NULL) {
            *response_len = strlen(*response);
        }
        return ret;
    } else if (strcmp(uri, "/api/bq25895/config") == 0) {
        if (strcmp(method, "GET") == 0) {
            // Get BQ25895 configuration
            esp_err_t ret = bq25895_get_params_json(response);
            if (ret == ESP_OK && *response != NULL) {
                *response_len = strlen(*response);
            }
            return ret;
        } else if (strcmp(method, "POST") == 0) {
            // Set BQ25895 configuration
            esp_err_t ret = bq25895_set_params_from_json(content);
            
            // Create response
            cJSON *json = cJSON_CreateObject();
            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(json, "success", true);
            } else {
                cJSON_AddBoolToObject(json, "success", false);
                cJSON_AddStringToObject(json, "message", "Failed to set configuration");
            }
            
            *response = cJSON_PrintUnformatted(json);
            if (*response != NULL) {
                *response_len = strlen(*response);
            }
            cJSON_Delete(json);
            
            return ret;
        }
    } else if (strcmp(uri, "/api/bq25895/reset") == 0 && strcmp(method, "POST") == 0) {
        // Reset BQ25895
        esp_err_t ret = bq25895_reset();
        
        // Create response
        cJSON *json = cJSON_CreateObject();
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(json, "success", true);
        } else {
            cJSON_AddBoolToObject(json, "success", false);
            cJSON_AddStringToObject(json, "message", "Failed to reset device");
        }
        
        *response = cJSON_PrintUnformatted(json);
        if (*response != NULL) {
            *response_len = strlen(*response);
        }
        cJSON_Delete(json);
        
        return ret;
    } else if (strcmp(uri, "/api/bq25895/ce_pin") == 0 && strcmp(method, "POST") == 0) {
        // Parse JSON content
        cJSON *json = cJSON_Parse(content);
        if (json == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            
            // Create error response
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", false);
            cJSON_AddStringToObject(resp, "message", "Invalid JSON format");
            
            *response = cJSON_PrintUnformatted(resp);
            if (*response != NULL) {
                *response_len = strlen(*response);
            }
            cJSON_Delete(resp);
            
            return ESP_ERR_INVALID_ARG;
        }
        
        // Get enable parameter
        cJSON *enable_item = cJSON_GetObjectItem(json, "enable");
        if (!enable_item || !cJSON_IsBool(enable_item)) {
            ESP_LOGE(TAG, "Missing or invalid 'enable' parameter");
            
            // Create error response
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", false);
            cJSON_AddStringToObject(resp, "message", "Missing or invalid 'enable' parameter");
            
            *response = cJSON_PrintUnformatted(resp);
            if (*response != NULL) {
                *response_len = strlen(*response);
            }
            cJSON_Delete(resp);
            cJSON_Delete(json);
            
            return ESP_ERR_INVALID_ARG;
        }
        
        // Set CE pin
        bool enable = cJSON_IsTrue(enable_item);
        esp_err_t ret = bq25895_integration_set_ce_pin(enable);
        
        // Create response
        cJSON *resp = cJSON_CreateObject();
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(resp, "success", true);
        } else {
            cJSON_AddBoolToObject(resp, "success", false);
            cJSON_AddStringToObject(resp, "message", "Failed to set CE pin");
        }
        
        *response = cJSON_PrintUnformatted(resp);
        if (*response != NULL) {
            *response_len = strlen(*response);
        }
        cJSON_Delete(resp);
        cJSON_Delete(json);
        
        return ret;
    } else if (strncmp(uri, "/api/bq25895/register", 20) == 0) {
        if (strcmp(method, "GET") == 0) {
            // Parse address parameter from query string
            const char *query = strchr(uri, '?');
            if (!query) {
                ESP_LOGE(TAG, "Missing address parameter");
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Missing address parameter");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Skip the '?' character
            query++;
            
            // Find the address parameter
            const char *addr_param = strstr(query, "address=");
            if (!addr_param) {
                ESP_LOGE(TAG, "Missing address parameter");
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Missing address parameter");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Skip the "address=" part
            addr_param += 8;
            
            // Parse the address value
            char *end;
            long addr = strtol(addr_param, &end, 10);
            if (addr_param == end || addr < 0 || addr > 0xFF) {
                ESP_LOGE(TAG, "Invalid address parameter: %s", addr_param);
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Invalid address parameter");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Read the register
            uint8_t value;
            esp_err_t ret = bq25895_read_reg((bq25895_reg_t)addr, &value);
            
            // Create response
            cJSON *resp = cJSON_CreateObject();
            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(resp, "success", true);
                cJSON_AddNumberToObject(resp, "address", addr);
                cJSON_AddNumberToObject(resp, "value", value);
            } else {
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Failed to read register");
            }
            
            *response = cJSON_PrintUnformatted(resp);
            if (*response != NULL) {
                *response_len = strlen(*response);
            }
            cJSON_Delete(resp);
            
            return ret;
        } else if (strcmp(method, "POST") == 0) {
            return 0;
            // Parse JSON content
            cJSON *json = cJSON_Parse(content);
            if (json == NULL) {
                ESP_LOGE(TAG, "Failed to parse JSON");
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Invalid JSON format");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Get address parameter
            cJSON *addr_item = cJSON_GetObjectItem(json, "address");
            if (!addr_item || !cJSON_IsNumber(addr_item)) {
                ESP_LOGE(TAG, "Missing or invalid 'address' parameter");
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Missing or invalid 'address' parameter");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                cJSON_Delete(json);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Get value parameter
            cJSON *value_item = cJSON_GetObjectItem(json, "value");
            if (!value_item || !cJSON_IsNumber(value_item)) {
                ESP_LOGE(TAG, "Missing or invalid 'value' parameter");
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Missing or invalid 'value' parameter");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                cJSON_Delete(json);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Check if address and value are in valid range
            int addr = addr_item->valueint;
            int value = value_item->valueint;
            
            if (addr < 0 || addr > 0xFF) {
                ESP_LOGE(TAG, "Invalid address: %d", addr);
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Invalid address (must be 0-255)");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                cJSON_Delete(json);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            if (value < 0 || value > 0xFF) {
                ESP_LOGE(TAG, "Invalid value: %d", value);
                
                // Create error response
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Invalid value (must be 0-255)");
                
                *response = cJSON_PrintUnformatted(resp);
                if (*response != NULL) {
                    *response_len = strlen(*response);
                }
                cJSON_Delete(resp);
                cJSON_Delete(json);
                
                return ESP_ERR_INVALID_ARG;
            }
            
            // Write the register
            esp_err_t ret = bq25895_write_reg((bq25895_reg_t)addr, (uint8_t)value);
            
            // Create response
            cJSON *resp = cJSON_CreateObject();
            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(resp, "success", true);
                cJSON_AddNumberToObject(resp, "address", addr);
                cJSON_AddNumberToObject(resp, "value", value);
            } else {
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "message", "Failed to write register");
            }
            
            *response = cJSON_PrintUnformatted(resp);
            if (*response != NULL) {
                *response_len = strlen(*response);
            }
            cJSON_Delete(resp);
            cJSON_Delete(json);
            
            return ret;
        }
    }

    // Unsupported request
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "message", "Unsupported request");
    
    *response = cJSON_PrintUnformatted(json);
    if (*response != NULL) {
        *response_len = strlen(*response);
    }
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief Get BQ25895 status as JSON
 */
static esp_err_t bq25895_get_status_json(char **json_str)
{
    bq25895_status_t status;
    esp_err_t ret = bq25895_get_status(&status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get BQ25895 status, err = %d", ret);
        
        // Create error response
        cJSON *json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "Failed to get BQ25895 status");
        
        *json_str = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        
        return ESP_OK; // Return OK to send the error response
    }

    cJSON *json = cJSON_CreateObject();
    
    // Add success flag
    cJSON_AddBoolToObject(json, "success", true);
    
    // Add status values to JSON
    cJSON_AddNumberToObject(json, "bat_voltage", status.bat_voltage);
    cJSON_AddNumberToObject(json, "sys_voltage", status.sys_voltage);
    cJSON_AddNumberToObject(json, "vbus_voltage", status.vbus_voltage);
    cJSON_AddNumberToObject(json, "charge_current", status.charge_current);
    cJSON_AddNumberToObject(json, "ts_voltage", status.ts_voltage);
    
    cJSON_AddNumberToObject(json, "vbus_stat", status.vbus_stat);
    cJSON_AddNumberToObject(json, "chg_stat", status.chg_stat);
    cJSON_AddBoolToObject(json, "pg_stat", status.pg_stat);
    cJSON_AddBoolToObject(json, "sdp_stat", status.sdp_stat);
    cJSON_AddBoolToObject(json, "vsys_stat", status.vsys_stat);
    
    cJSON_AddBoolToObject(json, "watchdog_fault", status.watchdog_fault);
    cJSON_AddBoolToObject(json, "boost_fault", status.boost_fault);
    cJSON_AddNumberToObject(json, "chg_fault", status.chg_fault);
    cJSON_AddBoolToObject(json, "bat_fault", status.bat_fault);
    cJSON_AddNumberToObject(json, "ntc_fault", status.ntc_fault);
    
    cJSON_AddBoolToObject(json, "therm_stat", status.therm_stat);
    
    *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief Get BQ25895 parameters as JSON
 */
static esp_err_t bq25895_get_params_json(char **json_str)
{
    bq25895_charge_params_t params;
    esp_err_t ret = bq25895_get_charge_params(&params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get BQ25895 parameters, err = %d", ret);
        
        // Create error response
        cJSON *json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "Failed to get BQ25895 parameters");
        
        *json_str = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        
        return ESP_OK; // Return OK to send the error response
    }

    cJSON *json = cJSON_CreateObject();
    
    // Add success flag
    cJSON_AddBoolToObject(json, "success", true);
    
    // Add parameter values to JSON
    cJSON_AddNumberToObject(json, "charge_voltage_mv", params.charge_voltage_mv);
    cJSON_AddNumberToObject(json, "charge_current_ma", params.charge_current_ma);
    cJSON_AddNumberToObject(json, "input_current_limit_ma", params.input_current_limit_ma);
    cJSON_AddNumberToObject(json, "input_voltage_limit_mv", params.input_voltage_limit_mv);
    cJSON_AddNumberToObject(json, "precharge_current_ma", params.precharge_current_ma);
    cJSON_AddNumberToObject(json, "termination_current_ma", params.termination_current_ma);
    cJSON_AddNumberToObject(json, "boost_voltage_mv", params.boost_voltage_mv);
    
    cJSON_AddNumberToObject(json, "thermal_regulation_threshold", params.thermal_regulation_threshold);
    cJSON_AddNumberToObject(json, "fast_charge_timer_hours", params.fast_charge_timer_hours);
    
    cJSON_AddBoolToObject(json, "enable_charging", params.enable_charging);
    cJSON_AddBoolToObject(json, "enable_otg", params.enable_otg);
    cJSON_AddBoolToObject(json, "enable_termination", params.enable_termination);
    cJSON_AddBoolToObject(json, "enable_safety_timer", params.enable_safety_timer);
    cJSON_AddBoolToObject(json, "enable_hi_impedance", params.enable_hi_impedance);
    cJSON_AddBoolToObject(json, "enable_ir_compensation", params.enable_ir_compensation);
    
    cJSON_AddNumberToObject(json, "ir_compensation_mohm", params.ir_compensation_mohm);
    cJSON_AddNumberToObject(json, "ir_compensation_voltage_mv", params.ir_compensation_voltage_mv);
    
    *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief Set BQ25895 parameters from JSON
 */
static esp_err_t bq25895_set_params_from_json(const char *json_str)
{
    return 0; // Disable setting params
    cJSON *json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    bq25895_charge_params_t params;
    esp_err_t ret = bq25895_get_charge_params(&params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get current BQ25895 parameters, err = %d", ret);
        cJSON_Delete(json);
        return ret;
    }

    // Update parameters from JSON
    cJSON *item;
    
    if ((item = cJSON_GetObjectItem(json, "charge_voltage_mv")) != NULL && cJSON_IsNumber(item)) {
        params.charge_voltage_mv = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "charge_current_ma")) != NULL && cJSON_IsNumber(item)) {
        params.charge_current_ma = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "input_current_limit_ma")) != NULL && cJSON_IsNumber(item)) {
        params.input_current_limit_ma = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "input_voltage_limit_mv")) != NULL && cJSON_IsNumber(item)) {
        params.input_voltage_limit_mv = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "precharge_current_ma")) != NULL && cJSON_IsNumber(item)) {
        params.precharge_current_ma = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "termination_current_ma")) != NULL && cJSON_IsNumber(item)) {
        params.termination_current_ma = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "boost_voltage_mv")) != NULL && cJSON_IsNumber(item)) {
        params.boost_voltage_mv = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "thermal_regulation_threshold")) != NULL && cJSON_IsNumber(item)) {
        params.thermal_regulation_threshold = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "fast_charge_timer_hours")) != NULL && cJSON_IsNumber(item)) {
        params.fast_charge_timer_hours = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "enable_charging")) != NULL && cJSON_IsBool(item)) {
        params.enable_charging = cJSON_IsTrue(item);
    }
    
    if ((item = cJSON_GetObjectItem(json, "enable_otg")) != NULL && cJSON_IsBool(item)) {
        params.enable_otg = cJSON_IsTrue(item);
    }
    
    if ((item = cJSON_GetObjectItem(json, "enable_termination")) != NULL && cJSON_IsBool(item)) {
        params.enable_termination = cJSON_IsTrue(item);
    }
    
    if ((item = cJSON_GetObjectItem(json, "enable_safety_timer")) != NULL && cJSON_IsBool(item)) {
        params.enable_safety_timer = cJSON_IsTrue(item);
    }
    
    if ((item = cJSON_GetObjectItem(json, "enable_hi_impedance")) != NULL && cJSON_IsBool(item)) {
        params.enable_hi_impedance = cJSON_IsTrue(item);
    }
    
    if ((item = cJSON_GetObjectItem(json, "enable_ir_compensation")) != NULL && cJSON_IsBool(item)) {
        params.enable_ir_compensation = cJSON_IsTrue(item);
    }
    
    if ((item = cJSON_GetObjectItem(json, "ir_compensation_mohm")) != NULL && cJSON_IsNumber(item)) {
        params.ir_compensation_mohm = item->valueint;
    }
    
    if ((item = cJSON_GetObjectItem(json, "ir_compensation_voltage_mv")) != NULL && cJSON_IsNumber(item)) {
        params.ir_compensation_voltage_mv = item->valueint;
    }
    
    cJSON_Delete(json);
    
    // Set updated parameters
    ret = bq25895_set_charge_params(&params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set BQ25895 parameters, err = %d", ret);
        return ret;
    }
    
    return ESP_OK;
}
