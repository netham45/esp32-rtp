/**
 * @file bq25895.h
 * @brief Driver for BQ25895 battery charger IC
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief BQ25895 I2C address */
#define BQ25895_I2C_ADDR                  0x6A

/** @brief Default I2C configuration for ESP32 */
#define BQ25895_DEFAULT_I2C_PORT          0        // I2C port number
#define BQ25895_DEFAULT_SCL_GPIO          9        // GPIO for I2C SCL
#define BQ25895_DEFAULT_SDA_GPIO          8        // GPIO for I2C SDA
#define BQ25895_DEFAULT_I2C_FREQ_HZ       100000   // 100kHz (can be up to 400kHz)
#define BQ25895_DEFAULT_I2C_TIMEOUT_MS    1000     // I2C timeout in milliseconds

/** @brief BQ25895 register addresses */
typedef enum {
    BQ25895_REG_00                     = 0x00,  /* Input Source Control */
    BQ25895_REG_01                     = 0x01,  /* Power-On Configuration */
    BQ25895_REG_02                     = 0x02,  /* Charge Current Control */
    BQ25895_REG_03                     = 0x03,  /* Pre-Charge/Termination Current Control */
    BQ25895_REG_04                     = 0x04,  /* Charge Voltage Control */
    BQ25895_REG_05                     = 0x05,  /* Charge Termination/Timer Control */
    BQ25895_REG_06                     = 0x06,  /* Boost Voltage/Thermal Regulation Control */
    BQ25895_REG_07                     = 0x07,  /* Misc Operation Control */
    BQ25895_REG_08                     = 0x08,  /* System Status */
    BQ25895_REG_09                     = 0x09,  /* Fault Status */
    BQ25895_REG_0A                     = 0x0A,  /* Voltage/Current Control */
    BQ25895_REG_0B                     = 0x0B,  /* System Status Register */
    BQ25895_REG_0C                     = 0x0C,  /* Fault Status Register */
    BQ25895_REG_0D                     = 0x0D,  /* VINDPM Threshold Setting */
    BQ25895_REG_0E                     = 0x0E,  /* Battery Voltage (ADC) */
    BQ25895_REG_0F                     = 0x0F,  /* System Voltage (ADC) */
    BQ25895_REG_10                     = 0x10,  /* TSPCT (ADC) */
    BQ25895_REG_11                     = 0x11,  /* VBUS Voltage (ADC) */
    BQ25895_REG_12                     = 0x12,  /* Charge Current (ADC) */
    BQ25895_REG_13                     = 0x13,  /* Input Current Limit Setting (PSEL) */
    BQ25895_REG_14                     = 0x14,  /* Device ID/Reset Control */
} bq25895_reg_t;

/** @brief Boost mode specific register aliases for clarity */
#define BQ25895_REG_CONTROL1           BQ25895_REG_03  /* OTG_CONFIG, CHG_CONFIG */
#define BQ25895_REG_BOOST_VOLTAGE      BQ25895_REG_0A  /* BOOSTV[3:0] bits 7-4 */
#define BQ25895_REG_SYSTEM_STATUS      BQ25895_REG_0B  /* VBUS_STAT, CHRG_STAT, PG_STAT */
#define BQ25895_REG_FAULT_STATUS       BQ25895_REG_0C  /* WATCHDOG_FAULT, BOOST_FAULT */

/** @brief Boost mode control bits */
#define BQ25895_OTG_CONFIG_BIT         (1 << 5)  /* Bit 5 in REG_03: Enable OTG */
#define BQ25895_CHG_CONFIG_BIT         (1 << 4)  /* Bit 4 in REG_03: Enable Charging */
#define BQ25895_WD_RST_BIT             (1 << 6)  /* Bit 6 in REG_03: Reset Watchdog */
#define BQ25895_BOOST_FAULT_BIT        (1 << 6)  /* Bit 6 in REG_0C: Boost Fault */
#define BQ25895_WATCHDOG_FAULT_BIT     (1 << 7)  /* Bit 7 in REG_0C: Watchdog Fault */

/** @brief BQ25895 charging status */
typedef enum {
    BQ25895_CHG_STAT_NOT_CHARGING      = 0,
    BQ25895_CHG_STAT_PRE_CHARGE        = 1,
    BQ25895_CHG_STAT_FAST_CHARGING     = 2,
    BQ25895_CHG_STAT_CHARGE_DONE       = 3,
} bq25895_chg_stat_t;

/** @brief BQ25895 VBUS status */
typedef enum {
    BQ25895_VBUS_STAT_NO_INPUT         = 0,
    BQ25895_VBUS_STAT_USB_HOST_SDP     = 1,
    BQ25895_VBUS_STAT_USB_CDP          = 2,
    BQ25895_VBUS_STAT_USB_DCP          = 3,
    BQ25895_VBUS_STAT_MAXCHARGE        = 4,
    BQ25895_VBUS_STAT_UNKNOWN_ADAPTER  = 5,
    BQ25895_VBUS_STAT_NON_STD_ADAPTER  = 6,
    BQ25895_VBUS_STAT_OTG              = 7,
} bq25895_vbus_stat_t;

/** @brief BQ25895 fault status */
typedef enum {
    BQ25895_FAULT_NORMAL               = 0,
    BQ25895_FAULT_INPUT                = 1,
    BQ25895_FAULT_THERMAL_SHUTDOWN     = 2,
    BQ25895_FAULT_TIMER_EXPIRED        = 3,
} bq25895_fault_t;

/** @brief BQ25895 NTC fault status */
typedef enum {
    BQ25895_NTC_NORMAL                 = 0,
    BQ25895_NTC_COLD                   = 1,
    BQ25895_NTC_HOT                    = 2,
} bq25895_ntc_fault_t;

/** @brief BQ25895 configuration structure */
typedef struct {
    int i2c_port;                         /*!< I2C port number */
    uint32_t i2c_freq;                    /*!< I2C frequency */
    int sda_gpio;                         /*!< GPIO for I2C SDA */
    int scl_gpio;                         /*!< GPIO for I2C SCL */
    int int_gpio;                         /*!< GPIO for INT pin, -1 if not used */
    int stat_gpio;                        /*!< GPIO for STAT pin, -1 if not used */
} bq25895_config_t;

/** @brief BQ25895 status structure */
typedef struct {
    bq25895_vbus_stat_t vbus_stat;        /*!< VBUS status */
    bq25895_chg_stat_t chg_stat;          /*!< Charging status */
    bool pg_stat;                         /*!< Power good status */
    bool sdp_stat;                        /*!< USB input status */
    bool vsys_stat;                       /*!< VSYS regulation status */
    bool watchdog_fault;                  /*!< Watchdog fault status */
    bool boost_fault;                     /*!< Boost mode fault status */
    bq25895_fault_t chg_fault;            /*!< Charge fault status */
    bool bat_fault;                       /*!< Battery fault status */
    bq25895_ntc_fault_t ntc_fault;        /*!< NTC fault status */
    bool therm_stat;                      /*!< Thermal regulation status */
    float bat_voltage;                    /*!< Battery voltage in volts */
    float sys_voltage;                    /*!< System voltage in volts */
    float vbus_voltage;                   /*!< VBUS voltage in volts */
    float charge_current;                 /*!< Charge current in amps */
    float ts_voltage;                     /*!< TS voltage as percentage of REGN */
} bq25895_status_t;

/** @brief BQ25895 charge parameters structure */
typedef struct {
    uint16_t charge_voltage_mv;           /*!< Charge voltage in mV (3840-4608mV) */
    uint16_t charge_current_ma;           /*!< Charge current in mA (0-5056mA) */
    uint16_t input_current_limit_ma;      /*!< Input current limit in mA (100-3250mA) */
    uint16_t input_voltage_limit_mv;      /*!< Input voltage limit in mV (3900-14000mV) */
    uint16_t precharge_current_ma;        /*!< Precharge current in mA (64-1024mA) */
    uint16_t termination_current_ma;      /*!< Termination current in mA (64-1024mA) */
    bool enable_termination;              /*!< Enable charge termination */
    bool enable_charging;                 /*!< Enable charging */
    bool enable_otg;                      /*!< Enable OTG mode */
    uint8_t thermal_regulation_threshold; /*!< Thermal regulation threshold (60/80/100/120°C) */
    uint8_t fast_charge_timer_hours;      /*!< Fast charge timer in hours (5/8/12/20) */
    bool enable_safety_timer;             /*!< Enable safety timer */
    bool enable_hi_impedance;             /*!< Enable Hi-Z mode */
    bool enable_ir_compensation;          /*!< Enable IR compensation */
    uint8_t ir_compensation_mohm;         /*!< IR compensation resistance in mOhm (0-140) */
    uint8_t ir_compensation_voltage_mv;   /*!< IR compensation voltage clamp in mV (0-224) */
    uint16_t boost_voltage_mv;            /*!< Boost mode voltage in mV (4550-5510) */
} bq25895_charge_params_t;

/**
 * @brief Initialize the BQ25895 driver
 * 
 * @param config Pointer to BQ25895 configuration structure
 * @return ESP_OK on success
 */
esp_err_t bq25895_init(const bq25895_config_t *config);

/**
 * @brief Deinitialize the BQ25895 driver
 * 
 * @return ESP_OK on success
 */
esp_err_t bq25895_deinit(void);

/**
 * @brief Reset the BQ25895 to default settings
 * 
 * @return ESP_OK on success
 */
esp_err_t bq25895_reset(void);

/**
 * @brief Get the current status of the BQ25895
 * 
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t bq25895_get_status(bq25895_status_t *status);

/**
 * @brief Get the current charge parameters of the BQ25895
 * 
 * @param params Pointer to charge parameters structure to fill
 * @return ESP_OK on success
 */
esp_err_t bq25895_get_charge_params(bq25895_charge_params_t *params);

/**
 * @brief Set the charge parameters of the BQ25895
 * 
 * @param params Pointer to charge parameters structure
 * @return ESP_OK on success
 */
esp_err_t bq25895_set_charge_params(const bq25895_charge_params_t *params);

/**
 * @brief Enable or disable charging
 * 
 * @param enable True to enable charging, false to disable
 * @return ESP_OK on success
 */
esp_err_t bq25895_enable_charging(bool enable);

/**
 * @brief Enable or disable OTG mode
 * 
 * @param enable True to enable OTG mode, false to disable
 * @return ESP_OK on success
 */
esp_err_t bq25895_enable_otg(bool enable);

/**
 * @brief Set the charge voltage limit
 * 
 * @param voltage_mv Charge voltage in mV (3840-4608mV)
 * @return ESP_OK on success
 */
esp_err_t bq25895_set_charge_voltage(uint16_t voltage_mv);

/**
 * @brief Set the charge current limit
 * 
 * @param current_ma Charge current in mA (0-5056mA)
 * @return ESP_OK on success
 */
esp_err_t bq25895_set_charge_current(uint16_t current_ma);

/**
 * @brief Set the input current limit
 * 
 * @param current_ma Input current limit in mA (100-3250mA)
 * @return ESP_OK on success
 */
esp_err_t bq25895_set_input_current_limit(uint16_t current_ma);

/**
 * @brief Set the input voltage limit
 * 
 * @param voltage_mv Input voltage limit in mV (3900-14000mV)
 * @return ESP_OK on success
 */
esp_err_t bq25895_set_input_voltage_limit(uint16_t voltage_mv);

/**
 * @brief Set the boost mode voltage
 * 
 * @param voltage_mv Boost mode voltage in mV (4550-5510)
 * @return ESP_OK on success
 */
esp_err_t bq25895_set_boost_voltage(uint16_t voltage_mv);

/**
 * @brief Reset the watchdog timer
 * 
 * @return ESP_OK on success
 */
esp_err_t bq25895_reset_watchdog(void);

/**
 * @brief Read a register from the BQ25895
 * 
 * @param reg Register address
 * @param value Pointer to store the register value
 * @return ESP_OK on success
 */
esp_err_t bq25895_read_reg(bq25895_reg_t reg, uint8_t *value);

/**
 * @brief Write a register to the BQ25895
 * 
 * @param reg Register address
 * @param value Value to write
 * @return ESP_OK on success
 */
esp_err_t bq25895_write_reg(bq25895_reg_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif
