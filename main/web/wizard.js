// ESP32 Scream Setup Wizard - Standalone Module

// ===== Wizard State =====
const wizard = {
    currentStep: 1,
    totalSteps: 5,
    data: {
        hostname: '',
        mode: '',
        ssid: '',
        password: '',
        apOnlyMode: false,
        receiverPort: 4010,
        senderDestination: '',
        senderPort: 4010,
        selectedReceiver: null,
        spdifDataPin: 17,
        apSettings: {
            ssid: 'ESP32-Scream',
            password: '',
            hideWhenConnected: false
        }
    },
    deviceCapabilities: {
        has_usb_capability: false,
        has_spdif_capability: false
    },
    discoveredReceivers: []
};

// ===== Communication with Parent Window =====
function sendMessageToParent(type, data) {
    if (window.parent !== window) {
        window.parent.postMessage({ type, data }, '*');
    }
}

// ===== Direct API Calls =====
function queueRequest(url, method = 'GET', body = null, headers = null) {
    // Always make direct fetch calls
    return fetch(url, { method, body });
}

// ===== Initialization =====
document.addEventListener('DOMContentLoaded', function() {
    // Initialize wizard
    wizard.currentStep = 1;
    updateWizardStep();
    
    // Fetch device capabilities
    fetchDeviceCapabilities();
    
    // Listen for messages from parent
    window.addEventListener('message', handleParentMessage);
    
    // Setup mode option click handlers
    setupModeClickHandlers();
});

function handleParentMessage(event) {
    if (event.data.type === 'init-wizard') {
        // Initialize wizard with data from parent
        if (event.data.capabilities) {
            wizard.deviceCapabilities = event.data.capabilities;
        }
        updateWizardStep();
    }
}

// ===== Step Content Functions =====
function updateWizardStep() {
    const backBtn = document.getElementById('wizard-back');
    const skipBtn = document.getElementById('wizard-skip');
    const nextBtn = document.getElementById('wizard-next');
    const progressFill = document.getElementById('wizard-progress-fill');
    const stepIndicator = document.getElementById('wizard-step-indicator');
    const footer = document.querySelector('.wizard-footer');
    
    // Determine total steps dynamically based on selected mode
    const isSpdifMode = (wizard.data.mode === 'receiver-spdif' || wizard.data.mode === 'sender-spdif');
    wizard.totalSteps = isSpdifMode ? 6 : 5;

    // Update progress
    const progressPercent = (wizard.currentStep / wizard.totalSteps) * 100;
    progressFill.style.width = `${progressPercent}%`;
    stepIndicator.textContent = `Step ${wizard.currentStep} of ${wizard.totalSteps}`;
    
    // Hide all steps and states
    hideAllSteps();
    
    // Update buttons
    backBtn.style.display = wizard.currentStep > 1 ? 'block' : 'none';
    skipBtn.style.display = 'none'; // Will be shown for specific steps
    nextBtn.textContent = 'Next'; // Reset button text
    footer.style.display = 'flex';
    
    // Show the current step (conditional mapping for SPDIF flow)
    switch(wizard.currentStep) {
        case 1:
            setupStep1_Mode();
            break;
        case 2:
            setupStep2_ModeSpecific();
            break;
        case 3:
            if (isSpdifMode) {
                setupStep3_SPDIFPin();
            } else {
                setupStep3_AP();
            }
            break;
        case 4:
            if (isSpdifMode) {
                setupStep3_AP();
            } else {
                setupStep4_NetworkConfig();
            }
            break;
        case 5:
            if (isSpdifMode) {
                setupStep4_NetworkConfig();
            } else {
                setupStep5_Review();
            }
            break;
        case 6:
            setupStep5_Review();
            break;
    }
}

function hideAllSteps() {
    // Hide all step divs
    document.querySelectorAll('.wizard-step').forEach(el => el.style.display = 'none');
    
    // Hide loading/success/error states
    document.getElementById('wizard-loading').style.display = 'none';
    document.getElementById('wizard-success').style.display = 'none';
    document.getElementById('wizard-error').style.display = 'none';
    document.getElementById('wifi-connecting-msg').style.display = 'none';
    document.getElementById('redirect-msg').style.display = 'none';
}

// Step 1: Mode Selection
function setupStep1_Mode() {
    document.getElementById('step-1').style.display = 'block';
    
    const hasUsb = wizard.deviceCapabilities.has_usb_capability;
    const hasSpdif = wizard.deviceCapabilities.has_spdif_capability;
    
    // Setup USB receiver mode
    const usbReceiverEl = document.getElementById('mode-receiver-usb');
    if (!hasUsb) {
        usbReceiverEl.classList.add('disabled');
        usbReceiverEl.querySelector('.mode-option-badge').style.display = 'inline';
        usbReceiverEl.querySelector('.mode-enabled-text').style.display = 'none';
        usbReceiverEl.querySelector('.mode-disabled-text').style.display = 'block';
    } else {
        usbReceiverEl.classList.remove('disabled');
        usbReceiverEl.querySelector('.mode-option-badge').style.display = 'none';
        usbReceiverEl.querySelector('.mode-enabled-text').style.display = 'block';
        usbReceiverEl.querySelector('.mode-disabled-text').style.display = 'none';
    }
    
    // Setup SPDIF receiver mode
    const spdifReceiverEl = document.getElementById('mode-receiver-spdif');
    if (!hasSpdif) {
        spdifReceiverEl.classList.add('disabled');
        spdifReceiverEl.querySelector('.mode-option-badge').style.display = 'inline';
        spdifReceiverEl.querySelector('.mode-enabled-text').style.display = 'none';
        spdifReceiverEl.querySelector('.mode-disabled-text').style.display = 'block';
    } else {
        spdifReceiverEl.classList.remove('disabled');
        spdifReceiverEl.querySelector('.mode-option-badge').style.display = 'none';
        spdifReceiverEl.querySelector('.mode-enabled-text').style.display = 'block';
        spdifReceiverEl.querySelector('.mode-disabled-text').style.display = 'none';
    }
    
    // Setup USB sender mode
    const usbSenderEl = document.getElementById('mode-sender-usb');
    if (!hasUsb) {
        usbSenderEl.classList.add('disabled');
        usbSenderEl.querySelector('.mode-option-badge').style.display = 'inline';
        usbSenderEl.querySelector('.mode-enabled-text').style.display = 'none';
        usbSenderEl.querySelector('.mode-disabled-text').style.display = 'block';
    } else {
        usbSenderEl.classList.remove('disabled');
        usbSenderEl.querySelector('.mode-option-badge').style.display = 'none';
        usbSenderEl.querySelector('.mode-enabled-text').style.display = 'block';
        usbSenderEl.querySelector('.mode-disabled-text').style.display = 'none';
    }
    
    // Setup SPDIF sender mode
    const spdifSenderEl = document.getElementById('mode-sender-spdif');
    if (!hasSpdif) {
        spdifSenderEl.classList.add('disabled');
        spdifSenderEl.querySelector('.mode-option-badge').style.display = 'inline';
        spdifSenderEl.querySelector('.mode-enabled-text').style.display = 'none';
        spdifSenderEl.querySelector('.mode-disabled-text').style.display = 'block';
    } else {
        spdifSenderEl.classList.remove('disabled');
        spdifSenderEl.querySelector('.mode-option-badge').style.display = 'none';
        spdifSenderEl.querySelector('.mode-enabled-text').style.display = 'block';
        spdifSenderEl.querySelector('.mode-disabled-text').style.display = 'none';
    }
    
    // Restore selected mode if any
    if (wizard.data.mode) {
        selectMode(wizard.data.mode);
    }
}

// Step 2: Mode-Specific Configuration
function setupStep2_ModeSpecific() {
    document.getElementById('step-2').style.display = 'block';
    
    const isSender = wizard.data.mode && (wizard.data.mode === 'sender-usb' || wizard.data.mode === 'sender-spdif');
    
    if (isSender) {
        // Show sender configuration
        document.getElementById('step-2-sender').style.display = 'block';
        document.getElementById('step-2-receiver').style.display = 'none';
        
        // Set existing values
        document.getElementById('sender-ip').value = wizard.data.senderDestination || '';
        document.getElementById('sender-port').value = wizard.data.senderPort || 4010;
        
        // Trigger receiver discovery
        discoverReceiversForWizard();
    } else {
        // Show receiver configuration
        document.getElementById('step-2-sender').style.display = 'none';
        document.getElementById('step-2-receiver').style.display = 'block';
        
        // Set existing values
        document.getElementById('receiver-port').value = wizard.data.receiverPort || 4010;
    }
}

// Step 3: S/PDIF Data Pin
function setupStep3_SPDIFPin() {
   document.getElementById('step-3-spdif').style.display = 'block';
   const pinInput = document.getElementById('spdif-data-pin');
   if (pinInput) {
       pinInput.value = (wizard.data.spdifDataPin !== undefined ? wizard.data.spdifDataPin : 17);
   }
}

// Step 3: AP Settings
function setupStep3_AP() {
   document.getElementById('step-3').style.display = 'block';
   const skipBtn = document.getElementById('wizard-skip');
   
   const isAPOnly = wizard.data.apOnlyMode;
   
   // Update UI based on AP mode
   document.getElementById('ap-optional-text').style.display = isAPOnly ? 'none' : 'inline';
   document.getElementById('ap-description').textContent = isAPOnly ?
       'Configure the device\'s WiFi access point that other devices will connect to:' :
       'Configure the device\'s own WiFi hotspot for initial setup or fallback access:';
   
   document.getElementById('ap-only-warning').style.display = isAPOnly ? 'block' : 'none';
   document.getElementById('ap-info').style.display = isAPOnly ? 'none' : 'block';
   document.getElementById('ap-hide-option').style.display = isAPOnly ? 'none' : 'block';
   
   // Allow skipping AP configuration except in AP-Only mode
   skipBtn.style.display = isAPOnly ? 'none' : 'block';
   
   // Set existing values
   document.getElementById('ap-ssid').value = wizard.data.apSettings.ssid;
   document.getElementById('ap-password').value = wizard.data.apSettings.password;
   document.getElementById('ap-hide-connected').checked = wizard.data.apSettings.hideWhenConnected;
}

// Step 4: Network Configuration
function setupStep4_NetworkConfig() {
    document.getElementById('step-4').style.display = 'block';
    
    // Set existing values
    document.getElementById('device-hostname').value = wizard.data.hostname || '';
    document.getElementById('wifi-ssid').value = wizard.data.ssid || '';
    document.getElementById('wifi-password').value = wizard.data.password || '';
    
    // Set network mode radio buttons
    const wifiRadio = document.querySelector('input[name="network-mode"][value="wifi"]');
    const apRadio = document.querySelector('input[name="network-mode"][value="ap-only"]');
    
    if (wizard.data.apOnlyMode) {
        apRadio.checked = true;
        document.getElementById('ap-mode-label').style.background = '#f0f8ff';
        document.getElementById('ap-mode-label').style.borderColor = '#007bff';
        document.getElementById('wifi-mode-label').style.background = '';
        document.getElementById('wifi-mode-label').style.borderColor = '#e0e0e0';
    } else {
        wifiRadio.checked = true;
        document.getElementById('wifi-mode-label').style.background = '#f0f8ff';
        document.getElementById('wifi-mode-label').style.borderColor = '#007bff';
        document.getElementById('ap-mode-label').style.background = '';
        document.getElementById('ap-mode-label').style.borderColor = '#e0e0e0';
    }
    
    // Show/hide sections based on mode
    toggleNetworkMode(wizard.data.apOnlyMode);
    
    // Update AP name preview if in AP-only mode
    if (wizard.data.apOnlyMode && wizard.data.apSettings.ssid) {
        document.getElementById('ap-name-preview').textContent = `Access Point Name: ${wizard.data.apSettings.ssid}`;
    }
}

// Step 5: Review & Finish
function setupStep5_Review() {
    document.getElementById('step-5').style.display = 'block';
    document.getElementById('wizard-next').textContent = 'Complete Setup';
    
    // Build summary list
    const summaryEl = document.getElementById('review-summary');
    let summaryHtml = '';
    
    // Device name
    summaryHtml += `<li><strong>Device Name:</strong> ${wizard.data.hostname}</li>`;
    
    // Network mode
    summaryHtml += `<li><strong>Network Mode:</strong> ${wizard.data.apOnlyMode ? '📡 AP-Only Mode' : '📶 WiFi Connected'}</li>`;
    
    // WiFi network (if not AP-only)
    if (!wizard.data.apOnlyMode) {
        summaryHtml += `<li><strong>WiFi Network:</strong> ${wizard.data.ssid}</li>`;
    }
    
    // Device mode
    summaryHtml += `<li><strong>Device Mode:</strong> ${formatModeName(wizard.data.mode)}</li>`;

    // Show S/PDIF data pin for SPDIF modes
    if (wizard.data.mode === 'receiver-spdif' || wizard.data.mode === 'sender-spdif') {
        const pin = (wizard.data.spdifDataPin !== undefined ? wizard.data.spdifDataPin : 17);
        summaryHtml += `<li><strong>S/PDIF Data Pin:</strong> ${pin}</li>`;
    }
    
    // Mode-specific item
    if (wizard.data.mode === 'receiver-usb' || wizard.data.mode === 'receiver-spdif') {
        summaryHtml += `<li><strong>Receiver Port:</strong> ${wizard.data.receiverPort}</li>`;
    } else if (wizard.data.mode === 'sender-usb' || wizard.data.mode === 'sender-spdif') {
        let target = '';
        if (wizard.data.selectedReceiver) {
            target = wizard.data.selectedReceiver.hostname || wizard.data.selectedReceiver.ip_address || 'Unknown Device';
        } else if (wizard.data.senderDestination === '239.255.77.77') {
            target = 'General RTP Multicast';
        } else if (wizard.data.senderDestination === '224.0.0.56') {
            target = 'PulseAudio RTP Multicast';
        } else {
            target = wizard.data.senderDestination || 'Custom';
        }
        summaryHtml += `<li><strong>Target Receiver:</strong> ${target} (${wizard.data.senderDestination}:${wizard.data.senderPort})</li>`;
    }
    
    // Access point settings
    summaryHtml += `<li><strong>Access Point Name:</strong> ${wizard.data.apSettings.ssid}</li>`;
    if (wizard.data.apOnlyMode) {
        summaryHtml += '<li><strong>Access Point:</strong> Always Active</li>';
    }
    
    summaryEl.innerHTML = summaryHtml;
    
    // Update reconnect warning
    const warningEl = document.getElementById('review-reconnect-warning');
    if (wizard.data.ssid) {
        warningEl.textContent = 'You may need to find its new IP address on your network to reconnect.';
    } else {
        warningEl.textContent = '';
    }
}

// ===== Helper Functions =====
function setupModeClickHandlers() {
    document.querySelectorAll('.mode-option').forEach(opt => {
        opt.addEventListener('click', function() {
            if (!this.classList.contains('disabled')) {
                selectMode(this.dataset.mode);
            }
        });
    });
}

function selectMode(mode) {
    // Clear previous selection
    document.querySelectorAll('.mode-option').forEach(opt => {
        opt.classList.remove('selected');
    });
    
    // Set new selection
    const selectedOption = document.querySelector(`.mode-option[data-mode="${mode}"]`);
    if (selectedOption && !selectedOption.classList.contains('disabled')) {
        selectedOption.classList.add('selected');
        wizard.data.mode = mode;
        
        // Clear error
        const errorEl = document.getElementById('mode-error');
        if (errorEl) errorEl.classList.remove('show');
    }
}

function selectReceiver(value) {
    // Clear manual IP checkbox when any dropdown option is selected
    const manualCheckbox = document.getElementById('manual-ip-check');
    if (manualCheckbox && value) {
        manualCheckbox.checked = false;
        toggleManualIP(false);
    }
    
    if (value && value !== 'manual' && !value.startsWith('multicast-')) {
        // Find receiver by matching the generated ID format (ip:port)
        const receiver = wizard.discoveredReceivers.find(r => {
            const deviceId = r.id || `${r.ip_address}:${r.port || 4010}`;
            return deviceId === value;
        });
        if (receiver) {
            wizard.data.selectedReceiver = receiver;
            wizard.data.senderDestination = receiver.ip_address;
            wizard.data.senderPort = receiver.port || 4010;

            // Update status - use hostname instead of name
            const statusEl = document.getElementById('receiver-status');
            if (statusEl) {
                statusEl.style.display = 'block';
                const deviceName = receiver.hostname || receiver.ip_address || 'Unknown Device';
                statusEl.innerHTML = `
                    <strong>Selected:</strong> ${deviceName}<br>
                    <strong>Address:</strong> ${receiver.ip_address}:${receiver.port || 4010}<br>
                    ${receiver.health_status ? `<strong>Status:</strong> ${receiver.health_status}` : ''}
                `;
            }
        }
    } else if (value === 'multicast-general') {
        // General RTP Multicast
        wizard.data.selectedReceiver = null;
        wizard.data.senderDestination = '239.255.77.77';
        wizard.data.senderPort = getRandomRTPPort();
        
        const statusEl = document.getElementById('receiver-status');
        if (statusEl) {
            statusEl.style.display = 'block';
            statusEl.innerHTML = `
                <strong>Selected:</strong> General RTP Multicast<br>
                <strong>Address:</strong> ${wizard.data.senderDestination}:${wizard.data.senderPort}<br>
                <strong>Mode:</strong> Auto-discovery multicast
            `;
        }
    } else if (value === 'multicast-pulseaudio') {
        // PulseAudio RTP Multicast
        wizard.data.selectedReceiver = null;
        wizard.data.senderDestination = '224.0.0.56';
        wizard.data.senderPort = getRandomRTPPort();
        
        const statusEl = document.getElementById('receiver-status');
        if (statusEl) {
            statusEl.style.display = 'block';
            statusEl.innerHTML = `
                <strong>Selected:</strong> PulseAudio RTP Multicast<br>
                <strong>Address:</strong> ${wizard.data.senderDestination}:${wizard.data.senderPort}<br>
                <strong>Mode:</strong> PulseAudio-compatible multicast
            `;
        }
    }
}

function toggleManualIP(checked) {
    const manualEntry = document.getElementById('manual-ip-entry');
    const receiverSelect = document.getElementById('receiver-select');
    
    if (manualEntry) {
        manualEntry.style.display = checked ? 'block' : 'none';
        if (receiverSelect) {
            receiverSelect.disabled = checked;
        }
    }
}

function toggleNetworkMode(isAPOnly) {
    wizard.data.apOnlyMode = isAPOnly;
    const wifiSection = document.getElementById('wifi-config-section');
    const apNotice = document.getElementById('ap-only-notice');
    
    if (wifiSection) {
        wifiSection.style.display = isAPOnly ? 'none' : 'block';
    }
    if (apNotice) {
        apNotice.style.display = isAPOnly ? 'block' : 'none';
        
        // Update AP name preview
        if (isAPOnly && wizard.data.apSettings.ssid) {
            document.getElementById('ap-name-preview').textContent = `Access Point Name: ${wizard.data.apSettings.ssid}`;
        }
    }
    
    // Update label styling
    if (isAPOnly) {
        document.getElementById('ap-mode-label').style.background = '#f0f8ff';
        document.getElementById('ap-mode-label').style.borderColor = '#007bff';
        document.getElementById('wifi-mode-label').style.background = '';
        document.getElementById('wifi-mode-label').style.borderColor = '#e0e0e0';
    } else {
        document.getElementById('wifi-mode-label').style.background = '#f0f8ff';
        document.getElementById('wifi-mode-label').style.borderColor = '#007bff';
        document.getElementById('ap-mode-label').style.background = '';
        document.getElementById('ap-mode-label').style.borderColor = '#e0e0e0';
    }
}

function formatModeName(mode) {
    const modeNames = {
        'receiver-usb': '🔊 USB Receiver',
        'receiver-spdif': '🔊 S/PDIF Receiver',
        'sender-usb': '🎤 USB Sender',
        'sender-spdif': '🎵 S/PDIF Sender',
        // Legacy numeric mappings
        0: '🔊 USB Receiver',           // MODE_RECEIVER_USB
        1: '🔊 S/PDIF Receiver',        // MODE_RECEIVER_SPDIF
        2: '🎤 USB Sender',             // MODE_SENDER_USB
        3: '🎵 S/PDIF Sender'           // MODE_SENDER_SPDIF
    };
    return modeNames[mode] || `Mode ${mode}`;
}

// ===== Network Discovery =====
function discoverReceiversForWizard() {
    const selectEl = document.getElementById('receiver-select');
    if (!selectEl) return;
    
    selectEl.innerHTML = '<option value="">-- Scanning... --</option>';
    selectEl.disabled = true;
    
    // Query mDNS discovered devices
    queueRequest('/api/scream_devices')
        .then(response => response.json())
        .then(data => {
            wizard.discoveredReceivers = data.devices || [];
            
            selectEl.innerHTML = '<option value="">-- Select a receiver --</option>';
            
            // Add multicast presets
            selectEl.innerHTML += '<optgroup label="Multicast Presets">';
            selectEl.innerHTML += '<option value="multicast-general">🌐 General RTP Multicast (239.255.77.77)</option>';
            selectEl.innerHTML += '<option value="multicast-pulseaudio">🎵 PulseAudio RTP Multicast (224.0.0.56)</option>';
            selectEl.innerHTML += '</optgroup>';
            
            if (wizard.discoveredReceivers.length > 0) {
                selectEl.innerHTML += '<optgroup label="Discovered Devices">';
                wizard.discoveredReceivers.forEach((device, index) => {
                    // Use hostname field and generate ID based on index or IP
                    const deviceId = device.id || `${device.ip_address}:${device.port || 4010}`;
                    const deviceName = device.hostname || device.ip_address || 'Unknown Device';

                    selectEl.innerHTML += `
                        <option value="${deviceId}">
                            ${deviceName} (${device.ip_address}:${device.port || 4010})
                        </option>
                    `;
                });
                selectEl.innerHTML += '</optgroup>';
            }
            
            selectEl.disabled = false;
            
            if (wizard.discoveredReceivers.length === 0) {
                const statusEl = document.getElementById('receiver-status');
                if (statusEl) {
                    statusEl.style.display = 'block';
                    statusEl.innerHTML = '⚠️ No receivers found. You can use multicast mode or enter IP manually.';
                }
            }
        })
        .catch(error => {
            console.error('Failed to discover receivers:', error);
            selectEl.innerHTML = '<option value="">-- Discovery failed --</option>';
            
            // Add multicast presets even when discovery fails
            selectEl.innerHTML += '<optgroup label="Multicast Presets">';
            selectEl.innerHTML += '<option value="multicast-general">🌐 General RTP Multicast (239.255.77.77)</option>';
            selectEl.innerHTML += '<option value="multicast-pulseaudio">🎵 PulseAudio RTP Multicast (224.0.0.56)</option>';
            selectEl.innerHTML += '</optgroup>';
            
            selectEl.disabled = false;
        });
}

function scanNetworksInWizard() {
    const listEl = document.getElementById('wizard-network-list');
    if (!listEl) return;
    
    listEl.innerHTML = '<div style="text-align: center; padding: 20px;">Scanning networks...</div>';
    
    queueRequest('/scan', 'GET')
        .then(response => response.json())
        .then(networks => {
            if (!networks || networks.length === 0) {
                listEl.innerHTML = '<div style="text-align: center; padding: 20px; color: #666;">No networks found</div>';
                return;
            }
            
            let html = '<div style="max-height: 200px; overflow-y: auto; border: 1px solid #ddd; border-radius: 4px; margin-top: 10px;">';
            networks.sort((a, b) => b.rssi - a.rssi);
            
            networks.forEach(network => {
                const signalStrength = network.rssi >= -50 ? 'excellent' :
                                      network.rssi >= -60 ? 'good' :
                                      network.rssi >= -70 ? 'fair' : 'weak';
                const lock = network.auth !== 0 ? '🔒' : '';
                
                html += `
                    <div style="padding: 10px; border-bottom: 1px solid #eee; cursor: pointer;"
                         onclick="selectWizardNetwork('${network.ssid}')">
                        <strong>${lock} ${network.ssid}</strong>
                        <span style="float: right; color: #666;">${network.rssi} dBm</span>
                    </div>
                `;
            });
            html += '</div>';
            
            listEl.innerHTML = html;
        })
        .catch(error => {
            listEl.innerHTML = `<div style="color: #dc3545; padding: 10px;">Failed to scan: ${error.message}</div>`;
        });
}

function selectWizardNetwork(ssid) {
    const ssidInput = document.getElementById('wifi-ssid');
    if (ssidInput) {
        ssidInput.value = ssid;
        wizard.data.ssid = ssid;
        document.getElementById('wifi-password').focus();
    }
}

// ===== Validation =====
function validateCurrentStep() {
    const isSpdifMode = (wizard.data.mode === 'receiver-spdif' || wizard.data.mode === 'sender-spdif');

    switch(wizard.currentStep) {
        case 1: // Mode Selection
            if (wizard.data.mode === null || wizard.data.mode === undefined || wizard.data.mode === '') {
                showWizardError('mode-error');
                return false;
            }
            break;
            
        case 2: // Mode-Specific Configuration
            if (wizard.data.mode === 'receiver-usb' || wizard.data.mode === 'receiver-spdif') {
                const port = parseInt(document.getElementById('receiver-port')?.value);
                if (isNaN(port) || port < 1 || port > 65535) {
                    showWizardError('receiver-error');
                    return false;
                }
                wizard.data.receiverPort = port;
            } else { // Sender modes
                const manualChecked = document.getElementById('manual-ip-check')?.checked;
                if (manualChecked) {
                    const ip = document.getElementById('sender-ip')?.value;
                    if (!ip || !validateIPAddress(ip)) {
                        showWizardError('sender-error', 'Please enter a valid IP address');
                        return false;
                    }
                    wizard.data.senderDestination = ip;
                    wizard.data.senderPort = parseInt(document.getElementById('sender-port')?.value) || 4010;
                } else if (!wizard.data.selectedReceiver && !wizard.data.senderDestination) {
                    showWizardError('sender-error');
                    return false;
                }
            }
            break;
            
        case 3:
            if (isSpdifMode) {
                const pin = parseInt(document.getElementById('spdif-data-pin')?.value);
                if (isNaN(pin) || pin < 0 || pin > 39) {
                    showWizardError('spdif-pin-error', 'Please enter a valid GPIO number (0-39)');
                    return false;
                }
                wizard.data.spdifDataPin = pin;
            } else {
                // AP Settings (non-SPDIF flow)
                wizard.data.apSettings.ssid = document.getElementById('ap-ssid')?.value || 'ESP32-Scream';
                wizard.data.apSettings.password = document.getElementById('ap-password')?.value || '';
                
                // For AP-Only mode, always keep AP visible
                if (wizard.data.apOnlyMode) {
                    wizard.data.apSettings.hideWhenConnected = false;
                } else {
                    wizard.data.apSettings.hideWhenConnected = document.getElementById('ap-hide-connected')?.checked || false;
                }
            }
            break;
            
        case 4:
            if (isSpdifMode) {
                // AP Settings (SPDIF flow)
                wizard.data.apSettings.ssid = document.getElementById('ap-ssid')?.value || 'ESP32-Scream';
                wizard.data.apSettings.password = document.getElementById('ap-password')?.value || '';
                
                if (wizard.data.apOnlyMode) {
                    wizard.data.apSettings.hideWhenConnected = false;
                } else {
                    wizard.data.apSettings.hideWhenConnected = document.getElementById('ap-hide-connected')?.checked || false;
                }
            } else {
                // Network Configuration (non-SPDIF flow)
                const hostnameInput = document.getElementById('device-hostname')?.value;
                if (!hostnameInput || hostnameInput.trim() === '') {
                    showWizardError('hostname-error', 'Please enter a device name');
                    return false;
                }
                wizard.data.hostname = hostnameInput.trim();

                const networkMode = document.querySelector('input[name="network-mode"]:checked')?.value;
                wizard.data.apOnlyMode = (networkMode === 'ap-only');

                if (!wizard.data.apOnlyMode) {
                    // WiFi mode - require SSID
                    const ssid = document.getElementById('wifi-ssid')?.value;
                    if (!ssid) {
                        showWizardError('wifi-error', 'Please enter a network name (SSID)');
                        return false;
                    }
                    wizard.data.ssid = ssid;
                    wizard.data.password = document.getElementById('wifi-password')?.value || '';
                } else {
                    // AP-Only mode - clear WiFi credentials
                    wizard.data.ssid = '';
                    wizard.data.password = '';
                }
            }
            break;
            
        case 5:
            if (isSpdifMode) {
                // Network Configuration (SPDIF flow)
                const hostnameInput = document.getElementById('device-hostname')?.value;
                if (!hostnameInput || hostnameInput.trim() === '') {
                    showWizardError('hostname-error', 'Please enter a device name');
                    return false;
                }
                wizard.data.hostname = hostnameInput.trim();

                const networkMode = document.querySelector('input[name="network-mode"]:checked')?.value;
                wizard.data.apOnlyMode = (networkMode === 'ap-only');

                if (!wizard.data.apOnlyMode) {
                    // WiFi mode - require SSID
                    const ssid = document.getElementById('wifi-ssid')?.value;
                    if (!ssid) {
                        showWizardError('wifi-error', 'Please enter a network name (SSID)');
                        return false;
                    }
                    wizard.data.ssid = ssid;
                    wizard.data.password = document.getElementById('wifi-password')?.value || '';
                } else {
                    // AP-Only mode - clear WiFi credentials
                    wizard.data.ssid = '';
                    wizard.data.password = '';
                }
            }
            // Non-SPDIF flow: step 5 is review, no validation
            break;

        case 6:
            // SPDIF flow: review step - no validation
            break;
    }
    
    return true;
}

function validateIPAddress(ip) {
    const regex = /^(\d{1,3}\.){3}\d{1,3}$/;
    if (!regex.test(ip)) return false;
    
    const parts = ip.split('.');
    for (const part of parts) {
        if (parseInt(part) > 255) return false;
    }
    
    return true;
}

function showWizardError(elementId, message) {
    const errorEl = document.getElementById(elementId);
    if (errorEl) {
        if (message) {
            errorEl.textContent = message;
        }
        errorEl.classList.add('show');
        setTimeout(() => errorEl.classList.remove('show'), 3000);
    }
}

// ===== Network Connection =====
function connectToNetwork() {
    const ssid = wizard.data.ssid || '';
    const password = wizard.data.password || '';

    const formData = new URLSearchParams();
    formData.append('ssid', ssid);
    formData.append('password', password);

    queueRequest('/connect', 'POST', formData)
        .then(response => {
            if (response.ok) {
                console.log('Connection request submitted');
            } else {
                console.error('Connection request failed');
            }
        })
        .catch(error => {
            console.error('Connection request error:', error);
        });
}

// ===== Navigation =====
function wizardNext() {
    if (!validateCurrentStep()) {
        return;
    }
    
    if (wizard.currentStep < wizard.totalSteps) {
        wizard.currentStep++;
        updateWizardStep();
    } else {
        // Complete wizard
        completeWizard();
    }
}

function wizardPrevious() {
    if (wizard.currentStep > 1) {
        wizard.currentStep--;
        updateWizardStep();
    }
}

function wizardSkip() {
    // Skip AP settings (step index depends on mode)
    const isSpdifMode = (wizard.data.mode === 'receiver-spdif' || wizard.data.mode === 'sender-spdif');
    const apStepNumber = isSpdifMode ? 4 : 3;
    if (wizard.currentStep === apStepNumber) {
        wizard.currentStep++;
        updateWizardStep();
    }
}

function closeWizard() {
    // Notify parent window
    sendMessageToParent('wizard-closed', {});
    
    // If in iframe, parent will handle closing
    if (window.parent === window) {
        // Not in iframe, close overlay
        const overlay = document.getElementById('setup-wizard-overlay');
        if (overlay) {
            overlay.classList.remove('active');
        }
    }
}

// ===== Completion =====
function completeWizard() {
    const footer = document.querySelector('.wizard-footer');
    
    // Hide all steps and show loading
    hideAllSteps();
    document.getElementById('wizard-loading').style.display = 'block';
    footer.style.display = 'none';
    
    // Map wizard mode strings to device_mode_t enum values
    const modeMap = {
        'receiver-usb': 0,       // MODE_RECEIVER_USB = 0
        'receiver-spdif': 1,     // MODE_RECEIVER_SPDIF = 1
        'sender-usb': 2,         // MODE_SENDER_USB = 2
        'sender-spdif': 3        // MODE_SENDER_SPDIF = 3
    };
    
    // Prepare settings payload - includes device's own AP settings, but NOT target WiFi credentials
    const settings = {
        setup_wizard_completed: true,
        hostname: wizard.data.hostname,
        device_mode: modeMap[wizard.data.mode] !== undefined ? modeMap[wizard.data.mode] : 0,
        // Device's own AP settings go through /api/settings
        ap_ssid: wizard.data.apSettings.ssid,
        ap_password: wizard.data.apSettings.password,
        spdif_data_pin: (wizard.data.spdifDataPin !== undefined ? wizard.data.spdifDataPin : 17),
        hide_ap_when_connected: wizard.data.apOnlyMode ? false : wizard.data.apSettings.hideWhenConnected
    };
    
    // Add mode-specific settings (audio configuration)
    if (wizard.data.mode === 'receiver-usb' || wizard.data.mode === 'receiver-spdif') {
        settings.port = wizard.data.receiverPort;
    } else {
        settings.sender_destination_ip = wizard.data.senderDestination || '239.255.77.77';
        settings.sender_destination_port = wizard.data.senderPort || 4010;
    }
    
    // NEVER include target WiFi credentials (ssid, password) in /api/settings
    // Those go through /connect endpoint ONLY
    
    // Save settings
    queueRequest('/api/settings', 'POST', JSON.stringify(settings), {
        'Content-Type': 'application/json'
    })
        .then(response => response.json())
        .then(result => {
            // IMMEDIATELY call /connect - NO DELAY!
            console.log('Settings saved, IMMEDIATELY calling /connect...');
            connectToNetwork();
            
            // Show success
            hideAllSteps();
            document.getElementById('wizard-success').style.display = 'block';
            
            // Show WiFi-specific message if applicable
            if (wizard.data.ssid) {
                document.getElementById('success-wifi-msg').style.display = 'block';
            }
            
            // Notify parent of completion
            sendMessageToParent('wizard-complete', settings);
            
            // Show appropriate message based on whether WiFi credentials were provided
            if (wizard.data.ssid && !wizard.data.apOnlyMode) {
                // Show connection progress for WiFi mode
                document.getElementById('wifi-connecting-msg').style.display = 'block';
                
                // Handle redirect after network change
                setTimeout(() => {
                    document.getElementById('redirect-msg').style.display = 'block';
                    
                    // Attempt to redirect after device restarts
                    setTimeout(() => {
                        if (window.parent === window) {
                            window.location.reload();
                        } else {
                            sendMessageToParent('wizard-redirect', {});
                        }
                    }, 10000);
                }, 5000);
            } else {
                // AP-only mode or no WiFi
                setTimeout(closeWizard, 3000);
            }
        })
        .catch(error => {
            hideAllSteps();
            document.getElementById('wizard-error').style.display = 'block';
            document.getElementById('error-message').textContent = error.message;
        });
}

// ===== Fetch Device Capabilities =====
function fetchDeviceCapabilities() {
    queueRequest('/api/settings')
        .then(response => response.json())
        .then(settings => {
            wizard.deviceCapabilities.has_usb_capability = settings.has_usb_capability || false;
            wizard.deviceCapabilities.has_spdif_capability = settings.has_spdif_capability || false;

            // Pre-populate wizard data with current settings
            wizard.data.hostname = settings.hostname || '';
            wizard.data.mode = getDeviceModeString(settings.device_mode);
            wizard.data.receiverPort = settings.port || 4010;
            wizard.data.senderDestination = settings.sender_destination_ip || '';
            wizard.data.senderPort = settings.sender_destination_port || 4010;
            wizard.data.spdifDataPin = (typeof settings.spdif_data_pin === 'number') ? settings.spdif_data_pin : 17;
            wizard.data.apSettings.ssid = settings.ap_ssid || 'ESP32-Scream';
            wizard.data.apSettings.password = settings.ap_password || '';
            wizard.data.apSettings.hideWhenConnected = settings.hide_ap_when_connected || false;

            // Get current WiFi credentials if available
            if (settings.ssid && settings.ssid.length > 0) {
                wizard.data.ssid = settings.ssid;
                wizard.data.password = settings.password || '';
                wizard.data.apOnlyMode = false; // If we have an SSID, we're not in AP-only mode
            } else {
                wizard.data.apOnlyMode = false; // Default to WiFi mode
            }

            // Always refresh current step content to show pre-populated values
            updateWizardStep();
        })
        .catch(error => {
            console.error('Failed to fetch device capabilities:', error);
        });
}

// Helper function to convert device_mode enum to string
function getDeviceModeString(modeEnum) {
    const modeMap = {
        0: 'receiver-usb',
        1: 'receiver-spdif',
        2: 'sender-usb',
        3: 'sender-spdif'
    };
    return modeMap[modeEnum] || '';
}

// Helper function to generate a random RTP port
function getRandomRTPPort() {
    // Use port range 5000-9999 for RTP (common practice)
    // Ensure it's an even number (RTP convention)
    const min = 5000;
    const max = 9998;
    const port = Math.floor(Math.random() * (max - min) / 2) * 2 + min;
    return port;
}

// Make functions available globally
window.selectMode = selectMode;
window.selectReceiver = selectReceiver;
window.toggleManualIP = toggleManualIP;
window.toggleNetworkMode = toggleNetworkMode;
window.discoverReceiversForWizard = discoverReceiversForWizard;
window.scanNetworksInWizard = scanNetworksInWizard;
window.selectWizardNetwork = selectWizardNetwork;
window.wizardNext = wizardNext;
window.wizardPrevious = wizardPrevious;
window.wizardSkip = wizardSkip;
window.closeWizard = closeWizard;