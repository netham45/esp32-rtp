# Set up web files for embedding into the firmware
set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_FILES
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/styles.css"
    "${WEB_SRC_DIR}/script.js"
    "${WEB_SRC_DIR}/spdif_stats.html"
    "${WEB_SRC_DIR}/spdif_stats.js"
    "${WEB_SRC_DIR}/wizard.js"
    "${WEB_SRC_DIR}/wizard.html"
    "${WEB_SRC_DIR}/wizard.css"
)

# BQ25895 battery charger files
set(BQ25895_SRCS
    "bq25895/bq25895.c"
    "bq25895/bq25895_web.c"
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

set(SENDER_SRCS
    "sender/spdif_in/histogram.c"
    "sender/spdif_in/spdif_in.c"
    "sender/usb_in.c"
)

set (RECEIVER_SRCS
    "receiver/audio_out.c"
    "receiver/buffer.c"
    "receiver/network_in.c"
    "receiver/spdif_out.c"
    "receiver/usb_out.c"
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
                           ${BQ25895_SRCS}
                           ${OTA_SRCS}
                           ${LOGGING_SRCS}
                           ${SENDER_SRCS}
                           ${RECEIVER_SRCS}
                      INCLUDE_DIRS "."
                      EMBED_FILES ${WEB_FILES})

# Enable C++ exceptions if not already enabled globally
target_compile_options(${COMPONENT_LIB} PRIVATE "-fexceptions")
