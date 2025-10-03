# BQ25895 Battery Charger IC Driver

High-performance I2C driver for the Texas Instruments BQ25895 battery management IC. Provides comprehensive control over battery charging, OTG mode operation, voltage/current monitoring, and fault detection with configurable parameters via Kconfig.

Key files:
- [include/bq25895.h](include/bq25895.h)
- [bq25895.c](bq25895.c)
- [include/bq25895_integration.h](include/bq25895_integration.h)
- [bq25895_integration.c](bq25895_integration.c)
- [include/bq25895_json.h](include/bq25895_json.h)
- [bq25895_json.c](bq25895_json.c)
- [idf_component.yml](idf_component.yml)

Supported targets and IDF:
- ESP-IDF [>= 5.4] as declared in [idf_component.yml](idf_component.yml)
- Targets: esp32, esp32s2, esp32s3, esp32p4 (see [idf_component.yml](idf_component.yml))


## Features

- **Battery Charging Control**: Configurable charge voltage, current, pre-charge, and termination parameters
- **OTG Mode Support**: Enable boost mode to supply 5V on VBUS for USB OTG applications
- **Comprehensive Monitoring**: Real-time ADC readings for battery voltage, system voltage, VBUS voltage, and charge current
- **Input Management**: Dynamic input current and voltage limit control with VINDPM
- **Safety Features**: Thermal regulation, safety timer, watchdog timer, and fault detection
- **Status Reporting**: Detailed charge status, VBUS detection, and fault status monitoring
- **JSON API**: Built-in JSON serialization for web interface integration
- **Kconfig Configuration**: I2C pins, address, and frequency configurable at compile time


## Hardware Connection

### Required Connections
- **SDA**: I2C data line (default GPIO from Kconfig)
- **SCL**: I2C clock line (default GPIO from Kconfig)
- **VBAT**: Battery positive terminal
- **GND**: Common ground
- **VBUS**: USB/adapter input voltage (5V-14V)

### Optional Connections
- **INT**: Interrupt output pin (active low, optional)
- **STAT**: Charge status LED output (optional)
- **CE**: Charge enable pin (active low, optional via GPIO12 in integration layer)
- **TS**: Temperature sense for battery NTC thermistor

### I2C Pull-ups
Ensure proper pull-up resistors (typically 4.7kΩ) on SDA and SCL lines to VCC (3.3V).


## Configuration

### Kconfig Options

Configure the component via `idf.py menuconfig` under "BQ25895 Configuration":

```
CONFIG_BQ25895_I2C_ADDR        - I2C address (default: 0x6A)
CONFIG_BQ25895_I2C_PORT        - I2C port number (default: 0)
CONFIG_BQ25895_SCL_GPIO        - GPIO for I2C SCL
CONFIG_BQ25895_SDA_GPIO        - GPIO for I2C SDA
CONFIG_BQ25895_I2C_FREQ_HZ     - I2C frequency in Hz (default: 100000, max: 400000)
```

### Default Configuration Constants

```c
#define BQ25895_DEFAULT_I2C_TIMEOUT_MS    1000     // I2C timeout in milliseconds
#define BQ25895_I2C_ADDR                  CONFIG_BQ25895_I2C_ADDR
#define BQ25895_DEFAULT_I2C_PORT          CONFIG_BQ25895_I2C_PORT
#define BQ25895_DEFAULT_SCL_GPIO          CONFIG_BQ25895_SCL_GPIO
#define BQ25895_DEFAULT_SDA_GPIO          CONFIG_BQ25895_SDA_GPIO
#define BQ25895_DEFAULT_I2C_FREQ_HZ       CONFIG_BQ25895_I2C_FREQ_HZ
```


## Quick Start

### 1) Initialize the driver

```c
#include "bq25895.h"

void app_init(void) {
    bq25895_config_t config = {
        .i2c_port = BQ25895_DEFAULT_I2C_PORT,
        .i2c_freq = BQ25895_DEFAULT_I2C_FREQ_HZ,
        .sda_gpio = BQ25895_DEFAULT_SDA_GPIO,
        .scl_gpio = BQ25895_DEFAULT_SCL_GPIO,
        .int_gpio = -1,  // Not used
        .stat_gpio = -1  // Not used
    };
    
    ESP_ERROR_CHECK(bq25895_init(&config));
}
```

### 2) Configure charge parameters

```c
bq25895_charge_params_t params = {
    .charge_voltage_mv = 4200,        // 4.2V battery
    .charge_current_ma = 1000,        // 1A charge current
    .input_current_limit_ma = 2000,   // 2A input limit
    .input_voltage_limit_mv = 4500,   // 4.5V minimum input
    .precharge_current_ma = 128,      // 128mA precharge
    .termination_current_ma = 128,    // 128mA termination
    .enable_termination = true,
    .enable_charging = true,
    .enable_otg = false,
    .thermal_regulation_threshold = 3, // 120°C
    .fast_charge_timer_hours = 12,
    .enable_safety_timer = true
};

ESP_ERROR_CHECK(bq25895_set_charge_params(&params));
```

### 3) Monitor charging status

```c
void monitor_charging(void) {
    bq25895_status_t status;
    
    ESP_ERROR_CHECK(bq25895_get_status(&status));
    
    ESP_LOGI(TAG, "Charge Status: %s", 
             status.chg_stat == BQ25895_CHG_STAT_FAST_CHARGING ? "Charging" :
             status.chg_stat == BQ25895_CHG_STAT_CHARGE_DONE ? "Done" : "Not Charging");
    ESP_LOGI(TAG, "Battery Voltage: %.2fV", status.bat_voltage);
    ESP_LOGI(TAG, "Charge Current: %.2fA", status.charge_current);
}
```

### 4) Cleanup

```c
ESP_ERROR_CHECK(bq25895_deinit());
```


## Core API Reference

### Initialization Functions

- [`bq25895_init()`](include/bq25895.h:158): Initialize the BQ25895 driver with I2C configuration
- [`bq25895_deinit()`](include/bq25895.h:165): Deinitialize the driver and release I2C resources
- [`bq25895_reset()`](include/bq25895.h:172): Reset the BQ25895 to factory default settings

### Status and Monitoring

- [`bq25895_get_status()`](include/bq25895.h:180): Get comprehensive status including voltages, currents, and fault states
- [`bq25895_get_charge_params()`](include/bq25895.h:188): Read current charge parameter configuration
- [`bq25895_reset_watchdog()`](include/bq25895.h:259): Reset the watchdog timer to prevent timeout

### Charge Control

- [`bq25895_set_charge_params()`](include/bq25895.h:196): Configure all charge parameters at once
- [`bq25895_enable_charging()`](include/bq25895.h:204): Enable or disable battery charging
- [`bq25895_set_charge_voltage()`](include/bq25895.h:220): Set charge voltage limit (3840-4608mV)
- [`bq25895_set_charge_current()`](include/bq25895.h:228): Set charge current limit (0-5056mA)

### Input Control

- [`bq25895_set_input_current_limit()`](include/bq25895.h:236): Set input current limit (100-3250mA)
- [`bq25895_set_input_voltage_limit()`](include/bq25895.h:244): Set VINDPM threshold (3900-14000mV)

### OTG/Boost Mode

- [`bq25895_enable_otg()`](include/bq25895.h:212): Enable or disable OTG boost mode
- [`bq25895_set_boost_voltage()`](include/bq25895.h:252): Set boost output voltage (4550-5510mV)

### Register Access

- [`bq25895_read_reg()`](include/bq25895.h:268): Direct register read access
- [`bq25895_write_reg()`](include/bq25895.h:277): Direct register write access


## Integration Layer API

The integration layer provides higher-level functions with additional features:

- [`bq25895_integration_init()`](include/bq25895_integration.h:20): Initialize with web interface support
- [`bq25895_integration_get_status()`](include/bq25895_integration.h:28): Get status through integration layer
- [`bq25895_integration_get_charge_params()`](include/bq25895_integration.h:36): Get parameters through integration layer
- [`bq25895_integration_set_charge_params()`](include/bq25895_integration.h:44): Set parameters through integration layer
- [`bq25895_integration_reset()`](include/bq25895_integration.h:51): Reset device through integration layer
- [`bq25895_integration_set_ce_pin()`](include/bq25895_integration.h:59): Control CE pin via GPIO12
- [`bq25895_integration_read_register()`](include/bq25895_integration.h:64): Read register through integration layer
- [`bq25895_integration_write_register()`](include/bq25895_integration.h:69): Write register through integration layer


## JSON API

JSON serialization for web interface integration:

- [`bq25895_status_to_json()`](include/bq25895_json.h:17): Convert status structure to JSON
- [`bq25895_params_to_json()`](include/bq25895_json.h:26): Convert parameters structure to JSON
- [`bq25895_params_update_from_json()`](include/bq25895_json.h:35): Update parameters from JSON


## Data Structures

### bq25895_config_t
```c
typedef struct {
    int i2c_port;          // I2C port number
    uint32_t i2c_freq;     // I2C frequency in Hz
    int sda_gpio;          // GPIO for I2C SDA
    int scl_gpio;          // GPIO for I2C SCL
    int int_gpio;          // GPIO for INT pin, -1 if not used
    int stat_gpio;         // GPIO for STAT pin, -1 if not used
} bq25895_config_t;
```

### bq25895_status_t
```c
typedef struct {
    bq25895_vbus_stat_t vbus_stat;    // VBUS status (USB type detection)
    bq25895_chg_stat_t chg_stat;      // Charging status
    bool pg_stat;                     // Power good status
    bool sdp_stat;                    // USB input status
    bool vsys_stat;                   // VSYS regulation status
    bool watchdog_fault;              // Watchdog fault status
    bool boost_fault;                 // Boost mode fault status
    bq25895_fault_t chg_fault;        // Charge fault status
    bool bat_fault;                   // Battery fault status
    bq25895_ntc_fault_t ntc_fault;    // NTC fault status
    bool therm_stat;                  // Thermal regulation status
    float bat_voltage;                // Battery voltage in volts
    float sys_voltage;                // System voltage in volts
    float vbus_voltage;               // VBUS voltage in volts
    float charge_current;             // Charge current in amps
    float ts_voltage;                 // TS voltage as percentage of REGN
} bq25895_status_t;
```

### bq25895_charge_params_t
```c
typedef struct {
    uint16_t charge_voltage_mv;           // Charge voltage (3840-4608mV)
    uint16_t charge_current_ma;           // Charge current (0-5056mA)
    uint16_t input_current_limit_ma;      // Input current limit (100-3250mA)
    uint16_t input_voltage_limit_mv;      // Input voltage limit (3900-14000mV)
    uint16_t precharge_current_ma;        // Precharge current (64-1024mA)
    uint16_t termination_current_ma;      // Termination current (64-1024mA)
    bool enable_termination;              // Enable charge termination
    bool enable_charging;                 // Enable charging
    bool enable_otg;                      // Enable OTG mode
    uint8_t thermal_regulation_threshold; // Thermal threshold (0:60°C, 1:80°C, 2:100°C, 3:120°C)
    uint8_t fast_charge_timer_hours;      // Fast charge timer (5/8/12/20 hours)
    bool enable_safety_timer;             // Enable safety timer
    bool enable_hi_impedance;             // Enable Hi-Z mode
    bool enable_ir_compensation;          // Enable IR compensation
    uint8_t ir_compensation_mohm;         // IR compensation resistance (0-140mΩ)
    uint8_t ir_compensation_voltage_mv;   // IR compensation voltage clamp (0-224mV)
    uint16_t boost_voltage_mv;            // Boost mode voltage (4550-5510mV)
} bq25895_charge_params_t;
```

### Enumerations

```c
// Charging Status
typedef enum {
    BQ25895_CHG_STAT_NOT_CHARGING  = 0,
    BQ25895_CHG_STAT_PRE_CHARGE    = 1,
    BQ25895_CHG_STAT_FAST_CHARGING = 2,
    BQ25895_CHG_STAT_CHARGE_DONE   = 3
} bq25895_chg_stat_t;

// VBUS Status (USB Type Detection)
typedef enum {
    BQ25895_VBUS_STAT_NO_INPUT        = 0,
    BQ25895_VBUS_STAT_USB_HOST_SDP    = 1,  // Standard Downstream Port
    BQ25895_VBUS_STAT_USB_CDP         = 2,  // Charging Downstream Port
    BQ25895_VBUS_STAT_USB_DCP         = 3,  // Dedicated Charging Port
    BQ25895_VBUS_STAT_MAXCHARGE       = 4,  // Adjustable High Voltage DCP
    BQ25895_VBUS_STAT_UNKNOWN_ADAPTER = 5,
    BQ25895_VBUS_STAT_NON_STD_ADAPTER = 6,
    BQ25895_VBUS_STAT_OTG             = 7   // In OTG mode
} bq25895_vbus_stat_t;

// Fault Status
typedef enum {
    BQ25895_FAULT_NORMAL           = 0,
    BQ25895_FAULT_INPUT            = 1,
    BQ25895_FAULT_THERMAL_SHUTDOWN = 2,
    BQ25895_FAULT_TIMER_EXPIRED    = 3
} bq25895_fault_t;

// NTC Fault Status
typedef enum {
    BQ25895_NTC_NORMAL = 0,
    BQ25895_NTC_COLD   = 1,
    BQ25895_NTC_HOT    = 2
} bq25895_ntc_fault_t;
```


## Example Usage

### Basic Battery Charging

```c
#include "bq25895.h"

void setup_basic_charging(void) {
    // Initialize driver
    bq25895_config_t config = {
        .i2c_port = 0,
        .i2c_freq = 100000,
        .sda_gpio = GPIO_NUM_21,
        .scl_gpio = GPIO_NUM_22,
        .int_gpio = -1,
        .stat_gpio = -1
    };
    
    ESP_ERROR_CHECK(bq25895_init(&config));
    
    // Configure for 1S Li-ion battery
    ESP_ERROR_CHECK(bq25895_set_charge_voltage(4200));      // 4.2V
    ESP_ERROR_CHECK(bq25895_set_charge_current(1000));      // 1A
    ESP_ERROR_CHECK(bq25895_set_input_current_limit(2000)); // 2A from USB
    ESP_ERROR_CHECK(bq25895_enable_charging(true));
    
    // Monitor charging
    while (1) {
        bq25895_status_t status;
        ESP_ERROR_CHECK(bq25895_get_status(&status));
        
        if (status.chg_stat == BQ25895_CHG_STAT_CHARGE_DONE) {
            ESP_LOGI(TAG, "Charging complete!");
            break;
        }
        
        ESP_LOGI(TAG, "Battery: %.2fV, Current: %.2fA", 
                 status.bat_voltage, status.charge_current);
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

### OTG Mode Operation

```c
void enable_otg_mode(void) {
    // Disable charging first
    ESP_ERROR_CHECK(bq25895_enable_charging(false));
    
    // Set boost voltage to 5.1V
    ESP_ERROR_CHECK(bq25895_set_boost_voltage(5100));
    
    // Enable OTG mode
    ESP_ERROR_CHECK(bq25895_enable_otg(true));
    
    // Monitor for faults
    bq25895_status_t status;
    ESP_ERROR_CHECK(bq25895_get_status(&status));
    
    if (status.boost_fault) {
        ESP_LOGE(TAG, "Boost fault detected!");
        ESP_ERROR_CHECK(bq25895_enable_otg(false));
    } else {
        ESP_LOGI(TAG, "OTG mode active, VBUS: %.2fV", status.vbus_voltage);
    }
}
```

### Dynamic Input Current Management

```c
void adapt_input_current(void) {
    bq25895_status_t status;
    ESP_ERROR_CHECK(bq25895_get_status(&status));
    
    // Adjust input current based on VBUS type
    uint16_t input_limit_ma;
    switch (status.vbus_stat) {
        case BQ25895_VBUS_STAT_USB_HOST_SDP:
            input_limit_ma = 500;   // USB 2.0 limit
            break;
        case BQ25895_VBUS_STAT_USB_CDP:
            input_limit_ma = 1500;  // USB BC1.2 CDP
            break;
        case BQ25895_VBUS_STAT_USB_DCP:
            input_limit_ma = 2000;  // USB DCP
            break;
        default:
            input_limit_ma = 1000;  // Conservative default
            break;
    }
    
    ESP_ERROR_CHECK(bq25895_set_input_current_limit(input_limit_ma));
    ESP_LOGI(TAG, "Input current limit set to %dmA", input_limit_ma);
}
```

### Fault Monitoring and Recovery

```c
void monitor_faults(void) {
    bq25895_status_t status;
    ESP_ERROR_CHECK(bq25895_get_status(&status));
    
    // Check watchdog fault
    if (status.watchdog_fault) {
        ESP_LOGW(TAG, "Watchdog fault - resetting");
        ESP_ERROR_CHECK(bq25895_reset_watchdog());
    }
    
    // Check charge fault
    if (status.chg_fault != BQ25895_FAULT_NORMAL) {
        switch (status.chg_fault) {
            case BQ25895_FAULT_INPUT:
                ESP_LOGE(TAG, "Input fault - check VBUS");
                break;
            case BQ25895_FAULT_THERMAL_SHUTDOWN:
                ESP_LOGE(TAG, "Thermal shutdown - reduce current");
                ESP_ERROR_CHECK(bq25895_set_charge_current(500));
                break;
            case BQ25895_FAULT_TIMER_EXPIRED:
                ESP_LOGE(TAG, "Safety timer expired");
                break;
        }
    }
    
    // Check NTC fault
    if (status.ntc_fault != BQ25895_NTC_NORMAL) {
        ESP_LOGW(TAG, "Battery temperature fault: %s",
                 status.ntc_fault == BQ25895_NTC_COLD ? "Too Cold" : "Too Hot");
    }
}
```

### Web Interface Integration

```c
#include "bq25895_integration.h"
#include "bq25895_json.h"

void handle_web_request(httpd_req_t *req) {
    bq25895_status_t status;
    ESP_ERROR_CHECK(bq25895_integration_get_status(&status));
    
    // Convert to JSON
    cJSON *json = bq25895_status_to_json(&status);
    char *json_str = cJSON_Print(json);
    
    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    // Cleanup
    free(json_str);
    cJSON_Delete(json);
}

void handle_parameter_update(httpd_req_t *req, cJSON *request_json) {
    bq25895_charge_params_t params;
    
    // Get current parameters
    ESP_ERROR_CHECK(bq25895_integration_get_charge_params(&params));
    
    // Update from JSON request
    ESP_ERROR_CHECK(bq25895_params_update_from_json(&params, request_json));
    
    // Apply new parameters
    ESP_ERROR_CHECK(bq25895_integration_set_charge_params(&params));
}
```


## Register Map

The BQ25895 has registers from 0x00 to 0x14:

| Register | Name | Description |
|----------|------|-------------|
| 0x00 | Input Source Control | Input current limit, VINDPM settings |
| 0x01 | Power-On Configuration | Watchdog, boost mode settings |
| 0x02 | Charge Current Control | Fast charge current limit |
| 0x03 | Pre-Charge/Termination | Pre-charge and termination current |
| 0x04 | Charge Voltage Control | Battery regulation voltage |
| 0x05 | Charge Timer Control | Safety timer, termination enable |
| 0x06 | Boost Voltage/Thermal | Boost voltage, thermal regulation |
| 0x07 | Misc Operation Control | BATFET, interrupts, timers |
| 0x08 | System Status | VBUS, charge status |
| 0x09 | Fault Status | Various fault flags |
| 0x0A | Voltage/Current Control | Force VINDPM, VINDPM threshold |
| 0x0B | System Status | PG, charging status details |
| 0x0C | Fault Status | NTC, fault details |
| 0x0D | VINDPM Threshold | Absolute VINDPM threshold |
| 0x0E | Battery Voltage ADC | Battery voltage reading |
| 0x0F | System Voltage ADC | System voltage reading |
| 0x10 | TS Percentage ADC | TS pin voltage |
| 0x11 | VBUS Voltage ADC | VBUS voltage reading |
| 0x12 | Charge Current ADC | Actual charge current |
| 0x13 | Input Current Limit | PSEL, input current settings |
| 0x14 | Device ID/Reset | Part number, revision, reset |


## Dependencies

- ESP-IDF >= 5.4
- I2C Master driver (built-in ESP-IDF component)
- cJSON (for JSON API functionality)
- FreeRTOS (built-in ESP-IDF component)


## Threading and Resources

- I2C operations use the ESP-IDF I2C master driver with configurable timeout
- All API functions are thread-safe when used with proper initialization
- No background tasks or timers are created by the driver itself
- Watchdog timer must be reset periodically by the application if enabled


## Limitations

- Maximum I2C frequency is 400kHz as per BQ25895 datasheet
- ADC conversion requires ~1ms after trigger for accurate readings
- Boost mode and charging cannot be enabled simultaneously
- Input current limit resolution is 50mA or 100mA depending on range
- Charge current resolution is 64mA
- Temperature sensing requires external NTC thermistor on TS pin


## Troubleshooting

### I2C Communication Fails
- Verify I2C pull-up resistors (4.7kΩ to 3.3V)
- Check GPIO assignments match hardware connections
- Confirm BQ25895 I2C address (default 0x6A, 0x6B if PMID tied high)
- Try reducing I2C frequency to 100kHz

### Charging Not Starting
- Check input voltage is above VINDPM threshold
- Verify battery voltage is within valid range (2.8V - 4.4V typically)
- Ensure CE pin is pulled low or controlled properly
- Check for fault conditions in status register

### OTG Mode Not Working
- Ensure charging is disabled before enabling OTG
- Check battery voltage is sufficient (>3.5V recommended)
- Monitor boost fault flag for overcurrent conditions
- Verify boost voltage setting is appropriate for load

### Incorrect ADC Readings
- Allow 1ms delay after triggering ADC conversion
- Check TS pin has proper NTC thermistor or resistor divider
- Verify REGN voltage is stable (typically 6V when powered)


## Version and Metadata

- Component version: 1.0.0
- Repository: [https://github.com/netham45/esp32-bq25895](https://github.com/netham45/esp32-bq25895)
- Supported targets: esp32, esp32s2, esp32s3, esp32p4
- License: See repository for license information