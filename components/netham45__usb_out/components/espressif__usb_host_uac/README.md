# USB Host UAC Driver with Apple USB DAC Support

[![Component Registry](https://components.espressif.com/components/espressif/usb_host_uac/badge.svg)](https://components.espressif.com/components/espressif/usb_host_uac)



## Overview

This driver implements a USB Audio Class (UAC) host driver on top of the [USB Host Library](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/peripherals/usb_host.html), allowing ESP32-S2, ESP32-S3, and ESP32-P4 devices to interact with USB audio devices.

The driver supports:
- UAC 1.0 compatible devices
- Special handling for Apple USB-C to 3.5mm Headphone Jack Adapter (VID=0x05AC, PID=0x110A)
- Extensible quirk system for handling non-standard USB audio devices

## Apple USB DAC Support

This fork adds support for the Apple USB-C to 3.5mm Headphone Jack Adapter through a quirk system. The adapter uses UAC 2.0 protocol, which is not fully supported by the original driver. The modifications include:

1. A quirk detection system that identifies specific USB devices by VID/PID
2. Special handling for the Apple adapter:
   - Forces configuration 2 for proper operation
   - Applies fixed endpoint parameters for reliable audio streaming
   - Enables proper volume and mute control

The quirk system is designed to be extensible, allowing for easy addition of support for other non-standard USB audio devices.

## Usage

The following steps outline the typical API call pattern of the UAC Class Driver:

1. Install the USB Host Library via `usb_host_install()`
2. Install the UAC driver via `uac_host_install()`
3. When the new (logic) UAC device is connected, the driver event callback will be called with USB device address and event:
    - `UAC_HOST_DRIVER_EVENT_TX_CONNECTED`
    - `UAC_HOST_DRIVER_EVENT_RX_CONNECTED`
4. To open/close the UAC device with USB device address and interface number:
    - `uac_host_device_open()`
    - `uac_host_device_close()`
5. To get the device-supported audio format use:
    - `uac_host_get_device_info()`
    - `uac_host_get_device_alt_param()`
6. To enable/disable data streaming with specific audio format use:
    - `uac_host_device_start()`
    - `uac_host_device_stop()`
7. To suspend/resume data streaming use:
    - `uac_host_device_suspend()`
    - `uac_host_device_resume()`
8. To control the volume/mute use:
    - `uac_host_device_set_mute()`
9. To control the volume use:
    - `uac_host_device_set_volume()` or `uac_host_device_set_volume_db()`
10. After the uac device is opened, the device event callback will be called with the following events:
    - UAC_HOST_DEVICE_EVENT_RX_DONE
    - UAC_HOST_DEVICE_EVENT_TX_DONE
    - UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR
    - UAC_HOST_DRIVER_EVENT_DISCONNECTED
11. When the `UAC_HOST_DRIVER_EVENT_DISCONNECTED` event is called, the device should be closed via `uac_host_device_close()`
12. The UAC driver can be uninstalled via `uac_host_uninstall()`

> Note: For physical device with both microphone and speaker, the driver will treat it as two separate logic devices.

> The `UAC_HOST_DRIVER_EVENT_TX_CONNECTED` and `UAC_HOST_DRIVER_EVENT_RX_CONNECTED` event will be called for the device.

## Quirk System

The quirk system allows for special handling of specific USB audio devices that don't fully comply with the UAC 1.0 standard or require special configuration. The system is implemented in `uac_quirks.c` and `uac_quirks.h`.

Currently supported quirks:
- **Apple USB-C to 3.5mm Headphone Jack Adapter** (VID=0x05AC, PID=0x110A)
  - Forces configuration 2
  - Sets fixed endpoint parameters for reliable audio streaming

To add support for additional devices, you can extend the `known_quirks` array in `uac_quirks.c`.

## Configuration Options

The component can be configured through Kconfig options:

- `PRINTF_UAC_CONFIGURATION_DESCRIPTOR`: Print UAC Configuration Descriptor to console
- `UAC_FREQ_NUM_MAX`: Maximum number of frequencies each Alt-interface supports
- `UAC_NUM_ISOC_URBS`: Number of UAC ISOC URBs to use
- `UAC_NUM_PACKETS_PER_URB`: Number of packets per UAC ISOC URB

## Examples

- For an example, refer to [usb_audio_player](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/host/usb_audio_player)

## Supported Devices

- UAC Driver supports any UAC 1.0 compatible device
- Apple USB-C to 3.5mm Headphone Jack Adapter (VID=0x05AC, PID=0x110A)
- Other devices can be supported by adding appropriate quirks