# Set up web files for embedding into the firmware
set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_FILES
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/styles.css"
    "${WEB_SRC_DIR}/script.js"
    "${WEB_SRC_DIR}/spdif_stats.html"
    "${WEB_SRC_DIR}/spdif_stats.js"
    "${WEB_SRC_DIR}/bq25895.html"
    "${WEB_SRC_DIR}/bq25895.css"
    "${WEB_SRC_DIR}/bq25895.js"
    "${WEB_SRC_DIR}/wizard.js"
    "${WEB_SRC_DIR}/wizard.html"
    "${WEB_SRC_DIR}/wizard.css"
    "${WEB_SRC_DIR}/sap.html"
    "${WEB_SRC_DIR}/sap.js"
)

# Web route module files
set(WEB_ROUTES_SRCS
    "web/routes/route_helpers.c"
    "web/routes/static_routes.c"
    "web/routes/wifi_routes.c"
    "web/routes/settings_routes.c"
    "web/routes/discovery_routes.c"
    "web/routes/pairing_routes.c"
    "web/routes/battery_routes.c"
    "web/routes/ota_routes.c"
    "web/routes/logs_routes.c"
    "web/routes/sap_routes.c"
    "web/routes/captive_portal_routes.c"
    "web/routes/routes.c"
)

# BQ25895 battery charger files
set(BQ25895_SRCS
    "bq25895/bq25895.c"
    "bq25895/bq25895_json.c"
    "bq25895/bq25895_integration.c"
)

# OTA manager files
set(OTA_SRCS
    "ota/ota_manager.c"
)

# Logging module files
set(LOGGING_SRCS
    "logging/log_buffer.c"
)

# Lifecycle manager module files
set(LIFECYCLE_SRCS
    "lifecycle/hw_init.c"
    "lifecycle/services.c"
    "lifecycle/sap.c"
    "lifecycle/config.c"
    "lifecycle/sleep.c"
    "lifecycle/reconfig.c"
    "lifecycle/modes.c"
    "lifecycle/state_machine.c"
)

set (RECEIVER_SRCS
    "receiver/audio_out.c"
    "receiver/buffer.c"
    "receiver/network_in.c"
    "receiver/sap_listener.c"
)

# Visualizer module files
set(VISUALIZER_SRCS
    "visualizer/pcm_visualizer.c"
    "visualizer/led_strip_controller.c"
    "visualizer/visualizer_task.c"
)

idf_component_register(SRCS "esp32-rtp.c"
                           "lifecycle_manager.c"
                           "config/config_manager.c"
                           "mdns/mdns_service.c"
                           "mdns/mdns_discovery.c"
                           "ntp/ntp_client.cpp"
                           "pairing/pairing_manager.c"
                           "sender/network_out.c"
                           "web/web_server.c"
                           "wifi/wifi_manager.c"
                           "TS3USB30ERSWR/usb_switch.c"
                           ${WEB_ROUTES_SRCS}
                           ${BQ25895_SRCS}
                           ${OTA_SRCS}
                           ${LOGGING_SRCS}
                           ${LIFECYCLE_SRCS}
                           ${RECEIVER_SRCS}
                           ${VISUALIZER_SRCS}
                      INCLUDE_DIRS "."
                      EMBED_FILES ${WEB_FILES})

