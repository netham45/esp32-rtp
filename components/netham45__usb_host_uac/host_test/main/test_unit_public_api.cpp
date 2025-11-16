/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/uac_host.h"
#include "usb/uac.h"
#include "mock_add_usb_device.h"

extern "C" {
#include "Mockusb_host.h"
}

SCENARIO("UAC Host pre-uninstall")
{
    // No UAC Driver installed
    GIVEN("NO UAC Host previously installed") {

        // Uninstall not-installed UAC Host driver
        SECTION("Uninstalling not installed UAC Host returns ESP_OK") {
            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == uac_host_uninstall());
        }
    }
}

SCENARIO("UAC Host install")
{
    uac_host_driver_config_t uac_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = (reinterpret_cast<uac_host_driver_event_cb_t>(0xdeadbeef)),
        .callback_arg = NULL
    };

    // UAC Host driver config set to nullptr
    GIVEN("NO UAC Host driver config, driver not already installed") {

        SECTION("Config is nullptr") {
            // Call the DUT function uac_host_install with uac_host_driver_config set to nullptr
            REQUIRE(ESP_ERR_INVALID_ARG == uac_host_install(nullptr));
        }
    }

    // UAC Host driver config set to config from UAC Host example,
    // but with various various changes causing UAC Host driver installation to fail
    GIVEN("Minimal UAC Host driver config, driver not already installed") {
        uac_host_driver_config.callback = nullptr;

        SECTION("Config error: no callback") {
            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == uac_host_install(&uac_host_driver_config));
        }

        SECTION("Config error: stack size is 0") {
            uac_host_driver_config.stack_size = 0;
            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == uac_host_install(&uac_host_driver_config));
        }

        SECTION("Config error: task priority is 0") {
            uac_host_driver_config.task_priority = 0;
            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == uac_host_install(&uac_host_driver_config));
        }
    }

    // UAC Host driver config set to config from UAC Host example
    GIVEN("Full UAC Host config, driver not already installed") {

        // Unable to register client: Invalid state, goto fail
        SECTION("Client register not successful: goto fail") {
            // Register a client, return ESP_ERR_INVALID_STATE, so the client is not registered successfully
            usb_host_client_register_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // goto fail: delete the semaphore

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == uac_host_install(&uac_host_driver_config));
        }

        // Call uac_host_install and expect successful installation
        SECTION("Client register successful: uac_host_install successful") {

            // Register a client, return ESP_OK, so the client is registered successfully
            usb_host_client_register_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_register_AddCallback(usb_host_client_register_mock_callback);

            // create USB Host Library client processing function
            usb_host_client_handle_events_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_handle_events_AddCallback(usb_host_client_handle_events_mock_callback);

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == uac_host_install(&uac_host_driver_config));
        }
    }

    // UAC Host driver config set to config from UAC Host example
    GIVEN("Full UAC Host config, driver already installed") {

        // Driver is already installed
        SECTION("Install error: driver already installed") {
            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == uac_host_install(&uac_host_driver_config));
        }
    }
}

SCENARIO("UAC Host post-uninstall")
{
    // UAC Host driver successfully installed
    GIVEN("UAC Host previously installed") {

        // UAC Driver is installed, uninstall the driver
        SECTION("UAC Host driver uninstall successful") {

            // Unblock client, return ESP_OK, register callback for unblocking the USB Host Library client processing function
            usb_host_client_unblock_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_unblock_AddCallback(usb_host_client_unblock_mock_callback);

            // Deregister the client
            usb_host_client_deregister_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_deregister_AddCallback(usb_host_client_deregister_mock_callback);

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == uac_host_uninstall());
        }
    }

    // UAC Host driver successfully uninstalled
    GIVEN("UAC Host successfully uninstalled") {

        // UAC Driver already successfully uninstalled, uninstall it again
        SECTION("Uninstall already uninstalled UAC Host driver") {
            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == uac_host_uninstall());
        }
    }
}
