document.addEventListener('DOMContentLoaded', function() {
    const refreshStatusBtn = document.getElementById('refresh-status');
    const loadConfigBtn = document.getElementById('load-config');
    const resetDeviceBtn = document.getElementById('reset-device');
    const configForm = document.getElementById('config-form');
    const cePinEnableBtn = document.getElementById('ce-pin-enable');
    const cePinDisableBtn = document.getElementById('ce-pin-disable');

    loadStatus();

    refreshStatusBtn.addEventListener('click', loadStatus);
    loadConfigBtn.addEventListener('click', loadConfig);
    resetDeviceBtn.addEventListener('click', resetDevice);
    cePinEnableBtn.addEventListener('click', () => setCePin(true));
    cePinDisableBtn.addEventListener('click', () => setCePin(false));
    configForm.addEventListener('submit', function(e) {
        e.preventDefault();
        saveConfig();
    });

    const readRegisterBtn = document.getElementById('read-register');
    const writeRegisterBtn = document.getElementById('write-register');
    const registerAddressInput = document.getElementById('register-address');
    const registerValueInput = document.getElementById('register-value');
    const registerResult = document.getElementById('register-result');

    readRegisterBtn.addEventListener('click', readRegister);
    writeRegisterBtn.addEventListener('click', writeRegister);

    function loadStatus() {
        fetch('/api/bq25895/status')
            .then(response => response.json())
            .then(data => {
                if (!data.success) {
                    console.error('Error loading status:', data.message);
                    alert('Failed to load status: ' + data.message);
                    return;
                }

                document.getElementById('bat-voltage').textContent = data.bat_voltage.toFixed(2) + ' V';
                document.getElementById('sys-voltage').textContent = data.sys_voltage.toFixed(2) + ' V';
                document.getElementById('vbus-voltage').textContent = data.vbus_voltage.toFixed(2) + ' V';
                document.getElementById('charge-current').textContent = data.charge_current.toFixed(2) + ' A';

                const chgStatusMap = ['Not Charging', 'Pre-charge', 'Fast Charging', 'Charge Done'];
                document.getElementById('charging-status').textContent = chgStatusMap[data.chg_stat] || 'Unknown';

                const vbusStatusMap = [
                    'No Input', 'USB Host SDP', 'USB CDP', 'USB DCP',
                    'MaxCharge', 'Unknown Adapter', 'Non-Standard Adapter', 'OTG'
                ];
                document.getElementById('vbus-status').textContent = vbusStatusMap[data.vbus_stat] || 'Unknown';

                document.getElementById('power-good').textContent = data.pg_stat ? 'Yes' : 'No';
                document.getElementById('thermal-status').textContent = data.therm_stat ? 'In Regulation' : 'Normal';

                document.getElementById('watchdog-fault').textContent = data.watchdog_fault ? 'Yes' : 'No';
                document.getElementById('boost-fault').textContent = data.boost_fault ? 'Yes' : 'No';

                const chgFaultMap = ['Normal', 'Input Fault', 'Thermal Shutdown', 'Timer Expired'];
                document.getElementById('charge-fault').textContent = chgFaultMap[data.chg_fault] || 'Normal';

                document.getElementById('battery-fault').textContent = data.bat_fault ? 'Yes' : 'No';

                const ntcFaultMap = ['Normal', 'Cold', 'Hot'];
                document.getElementById('ntc-fault').textContent = ntcFaultMap[data.ntc_fault] || 'Normal';
            })
            .catch(error => {
                console.error('Error loading status:', error);
                alert('Failed to load status. Please try again.');
            });
    }

    function loadConfig() {
        fetch('/api/bq25895/config')
            .then(response => response.json())
            .then(data => {
                if (!data.success) {
                    console.error('Error loading configuration:', data.message);
                    alert('Failed to load configuration: ' + data.message);
                    return;
                }

                document.getElementById('charge-voltage').value = data.charge_voltage_mv;
                document.getElementById('charge-current-input').value = data.charge_current_ma;
                document.getElementById('input-current-limit').value = data.input_current_limit_ma;
                document.getElementById('input-voltage-limit').value = data.input_voltage_limit_mv;
                document.getElementById('precharge-current').value = data.precharge_current_ma;
                document.getElementById('termination-current').value = data.termination_current_ma;
                document.getElementById('boost-voltage').value = data.boost_voltage_mv;
                document.getElementById('thermal-regulation').value = data.thermal_regulation_threshold;
                document.getElementById('fast-charge-timer').value = data.fast_charge_timer_hours;
                document.getElementById('enable-charging').checked = data.enable_charging;
                document.getElementById('enable-otg').checked = data.enable_otg;
                document.getElementById('enable-termination').checked = data.enable_termination;
                document.getElementById('enable-safety-timer').checked = data.enable_safety_timer;
                document.getElementById('enable-hi-impedance').checked = data.enable_hi_impedance;
                document.getElementById('enable-ir-compensation').checked = data.enable_ir_compensation;
                document.getElementById('ir-compensation-mohm').value = data.ir_compensation_mohm;
                document.getElementById('ir-compensation-voltage').value = data.ir_compensation_voltage_mv;
            })
            .catch(error => {
                console.error('Error loading configuration:', error);
                alert('Failed to load configuration. Please try again.');
            });
    }

    function saveConfig() {
        const config = {
            charge_voltage_mv: parseInt(document.getElementById('charge-voltage').value, 10),
            charge_current_ma: parseInt(document.getElementById('charge-current-input').value, 10),
            input_current_limit_ma: parseInt(document.getElementById('input-current-limit').value, 10),
            input_voltage_limit_mv: parseInt(document.getElementById('input-voltage-limit').value, 10),
            precharge_current_ma: parseInt(document.getElementById('precharge-current').value, 10),
            termination_current_ma: parseInt(document.getElementById('termination-current').value, 10),
            boost_voltage_mv: parseInt(document.getElementById('boost-voltage').value, 10),
            thermal_regulation_threshold: parseInt(document.getElementById('thermal-regulation').value, 10),
            fast_charge_timer_hours: parseInt(document.getElementById('fast-charge-timer').value, 10),
            enable_charging: document.getElementById('enable-charging').checked,
            enable_otg: document.getElementById('enable-otg').checked,
            enable_termination: document.getElementById('enable-termination').checked,
            enable_safety_timer: document.getElementById('enable-safety-timer').checked,
            enable_hi_impedance: document.getElementById('enable-hi-impedance').checked,
            enable_ir_compensation: document.getElementById('enable-ir-compensation').checked,
            ir_compensation_mohm: parseInt(document.getElementById('ir-compensation-mohm').value, 10),
            ir_compensation_voltage_mv: parseInt(document.getElementById('ir-compensation-voltage').value, 10)
        };

        fetch('/api/bq25895/config', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(config)
        })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    alert('Configuration saved successfully!');
                    loadStatus();
                } else {
                    alert('Failed to save configuration: ' + data.message);
                }
            })
            .catch(error => {
                console.error('Error saving configuration:', error);
                alert('Failed to save configuration. Please try again.');
            });
    }

    function resetDevice() {
        if (confirm('Are you sure you want to reset the BQ25895 device?')) {
            fetch('/api/bq25895/reset', {
                method: 'POST'
            })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        alert('Device reset successfully!');
                        loadStatus();
                        loadConfig();
                    } else {
                        alert('Failed to reset device: ' + data.message);
                    }
                })
                .catch(error => {
                    console.error('Error resetting device:', error);
                    alert('Failed to reset device. Please try again.');
                });
        }
    }

    function setCePin(enable) {
        fetch('/api/bq25895/ce_pin', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ enable })
        })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    alert(`CE pin ${enable ? 'enabled' : 'disabled'} successfully!`);
                    loadStatus();
                } else {
                    alert('Failed to set CE pin: ' + data.message);
                }
            })
            .catch(error => {
                console.error('Error setting CE pin:', error);
                alert('Failed to set CE pin. Please try again.');
            });
    }

    function readRegister() {
        const regAddress = parseHexInput(registerAddressInput.value);
        if (regAddress === null) {
            alert('Please enter a valid register address (0x00-0xFF)');
            return;
        }

        fetch(`/api/bq25895/register?address=${regAddress}`)
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    registerResult.textContent = `Read register 0x${regAddress.toString(16).padStart(2, '0').toUpperCase()}: 0x${data.value.toString(16).padStart(2, '0').toUpperCase()}`;
                    registerValueInput.value = '0x' + data.value.toString(16).padStart(2, '0').toUpperCase();
                } else {
                    registerResult.textContent = `Error: ${data.message}`;
                }
            })
            .catch(error => {
                console.error('Error reading register:', error);
                registerResult.textContent = 'Error: Failed to read register. Please try again.';
            });
    }

    function writeRegister() {
        const regAddress = parseHexInput(registerAddressInput.value);
        if (regAddress === null) {
            alert('Please enter a valid register address (0x00-0xFF)');
            return;
        }

        const regValue = parseHexInput(registerValueInput.value);
        if (regValue === null) {
            alert('Please enter a valid register value (0x00-0xFF)');
            return;
        }

        fetch('/api/bq25895/register', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ address: regAddress, value: regValue })
        })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    registerResult.textContent = `Wrote 0x${regValue.toString(16).padStart(2, '0').toUpperCase()} to register 0x${regAddress.toString(16).padStart(2, '0').toUpperCase()}`;
                } else {
                    registerResult.textContent = `Error: ${data.message}`;
                }
            })
            .catch(error => {
                console.error('Error writing register:', error);
                registerResult.textContent = 'Error: Failed to write register. Please try again.';
            });
    }

    function parseHexInput(input) {
        if (!input) {
            return null;
        }

        let sanitized = input.trim();
        if (sanitized.startsWith('0x') || sanitized.startsWith('0X')) {
            sanitized = sanitized.substring(2);
        }

        const value = parseInt(sanitized, 16);
        if (Number.isNaN(value) || value < 0 || value > 0xFF) {
            return null;
        }

        return value;
    }
});