# Changelog for USB Host UAC

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- USB Audio Class 2.0 host support including clock-entity parsing, programmable sample-rate control and richer descriptor metadata reported through `uac_host_dev_info_t`/`uac_host_dev_alt_param_t`.

### Changed

- `uac_host_dev_info_t` and `uac_host_dev_alt_param_t` now expose the reported spec version, format bitmap, channel configuration and subslot size so applications can differentiate UAC1 vs UAC2 devices.

## [1.3.2] - 2025-10-21

### Changed

- uac_host_device_read() now waits for the full timeout duration until the required amount of data is read, preventing premature returns with partial data (https://github.com/espressif/esp-usb/issues/248)

## [1.3.1] - 2025-09-24

- Added support for ESP32-H4

## [1.3.0] - 2025-03-28

- Added Linux target build for the UAC component, host tests (https://github.com/espressif/esp-usb/issues/143)

## [1.2.0] - 2024-10-10

### Breaking Changes:

- Changed the parameter type of `uac_host_device_set_volume_db` from uint32_t to int16_t

### Improvements:

- Support get current volume and mute status

### Bugfixes:

- Fixed incorrect volume conversion. Using actual device volume range.
- Fixed concurrency issues when suspend/stop during read/write

## [1.1.0] - 2024-07-08

### Improvements

- Added add `uac_host_device_open_with_vid_pid` to open connected audio devices with known VID and PID
- Print component version message `uac-host: Install Succeed, Version: 1.1.0` in `uac_host_install` function

## [1.0.0] - 2024-05-23

- Initial version
