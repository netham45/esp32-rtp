# USB Switch Integration Plan for ESP32-RTP Lifecycle Manager

## Overview
Integrate the TS3USB30ERSWR USB switch control into the lifecycle manager to automatically switch between USB ports based on the operational mode.

## Requirements
- **USB Sender Mode**: Switch to Port 2 (non-default state)
- **All Other Modes**: Switch to Port 1 (default state)

## Architecture Design

### 1. Component Dependencies
```
main/lifecycle/
├── hw_init.c        → Add USB switch initialization
├── hw_init.h        → Declare USB switch init function
└── modes.c          → Add USB switch control in mode transitions

components/netham45__TS3USB30ERSWR/
├── TS3USB30ERSWR.c  → Existing USB switch implementation
└── include/
    └── TS3USB30ERSWR.h → USB switch API
```

### 2. Implementation Strategy

#### Phase 1: Hardware Initialization
**File**: `main/lifecycle/hw_init.c`
- Add `lifecycle_hw_init_usb_switch()` function
- Initialize USB switch with default state (Port 1)
- Call from `handle_state_entry()` in `LIFECYCLE_STATE_HW_INIT`

#### Phase 2: USB Sender Mode Control
**File**: `main/lifecycle/modes.c`
- In `start_mode_sender_usb()`:
  - Add USB switch control to set Port 2
  - Log the switch state change
- In `stop_mode_sender_usb()`:
  - Add USB switch control to set Port 1
  - Log the switch state change

#### Phase 3: Other Modes Control
**File**: `main/lifecycle/modes.c`
- In `start_mode_sender_spdif()`, `start_mode_receiver_usb()`, `start_mode_receiver_spdif()`:
  - Ensure USB switch is set to Port 1
  - This provides redundancy and ensures correct state

### 3. Code Changes Summary

#### hw_init.h additions:
```c
esp_err_t lifecycle_hw_init_usb_switch(void);
```

#### hw_init.c additions:
```c
#include "TS3USB30ERSWR.h"

esp_err_t lifecycle_hw_init_usb_switch(void) {
    ESP_LOGI(TAG, "Initializing USB switch");
    esp_err_t ret = usb_switch_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB switch: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set to default state (Port 1)
    ret = usb_switch_set_port(USB_SWITCH_PORT_1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set USB switch to default port: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "USB switch initialized and set to Port 1 (default)");
    return ESP_OK;
}
```

#### modes.c modifications:
```c
#include "TS3USB30ERSWR.h"

// In start_mode_sender_usb():
// After line 73, add:
    // Switch USB to Port 2 for USB sender mode
    ESP_LOGI(TAG, "Setting USB switch to Port 2 for USB sender mode");
    esp_err_t ret = usb_switch_set_port(USB_SWITCH_PORT_2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set USB switch to Port 2: %s", esp_err_to_name(ret));
        // Non-critical, continue
    }

// In stop_mode_sender_usb():
// After line 125, add:
    // Switch USB back to Port 1 (default)
    ESP_LOGI(TAG, "Setting USB switch back to Port 1 (default)");
    ret = usb_switch_set_port(USB_SWITCH_PORT_1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set USB switch to Port 1: %s", esp_err_to_name(ret));
        // Non-critical, continue
    }

// In other mode start functions, ensure Port 1:
// Add after mode initialization:
    // Ensure USB switch is in default state (Port 1)
    usb_switch_set_port(USB_SWITCH_PORT_1);
```

### 4. Testing Plan
1. Verify USB switch initializes correctly on boot
2. Test mode transitions:
   - Enter USB sender mode → Verify Port 2 is selected
   - Exit USB sender mode → Verify Port 1 is selected
   - Enter other modes → Verify Port 1 is maintained
3. Check logs for proper state change messages
4. Test error handling if USB switch fails to initialize

### 5. Error Handling
- USB switch initialization failures are non-critical
- Mode operations continue even if switch fails
- All errors are logged for debugging

### 6. Logging Strategy
- Log all USB switch state changes
- Include port number in log messages
- Use INFO level for normal operations
- Use ERROR level for failures