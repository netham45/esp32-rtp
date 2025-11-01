# Auto-increment version before build
# Change 'build' to 'patch' if you want to increment patch instead
execute_process(
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/increment_version.py
            ${CMAKE_CURRENT_LIST_DIR}/version.h build
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    RESULT_VARIABLE VERSION_INCREMENT_RESULT
)

if(NOT VERSION_INCREMENT_RESULT EQUAL 0)
    message(WARNING "Failed to increment version")
endif()

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
    "lifecycle/lifecycle_wifi_adapter.c"
)

set (RECEIVER_SRCS
    "receiver/audio_out.c"
    "receiver/buffer.c"
    "receiver/network_in.c"
    "receiver/sap_listener.c"
    "receiver/rtcp_receiver.c"
)

set(OPUS_ROOT "${CMAKE_CURRENT_LIST_DIR}/deps/opus")
if(NOT EXISTS "${OPUS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Opus dependency not found at ${OPUS_ROOT}")
endif()

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Opus: build only the static library" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "Opus: disable tests" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "Opus: disable example programs" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "Opus: skip pkg-config files" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "Opus: skip CMake package config" FORCE)
    set(OPUS_DEEP_PLC OFF CACHE BOOL "Opus: disable Deep PLC (DNN) support" FORCE)
    set(OPUS_DRED OFF CACHE BOOL "Opus: disable DRED neural post-processing" FORCE)
    set(OPUS_OSCE OFF CACHE BOOL "Opus: disable OSCE neural enhancements" FORCE)
    set(OPUS_DNN_FLOAT_DEBUG OFF CACHE BOOL "Opus: keep DNN debug helpers disabled" FORCE)
    set(OPUS_VAR_ARRAYS ON CACHE BOOL "Opus: disable VLA stack allocations" FORCE)
    set(OPUS_USE_ALLOCA ON CACHE BOOL "Opus: disable alloca stack allocations" FORCE)
    set(OPUS_FIXED_POINT ON CACHE BOOL "Opus: Used Fixed Point" FORCE)
    set(OPUS_NONTHREADSAFE_PSEUDOSTACK OFF CACHE BOOL "Opus: use heap-backed pseudostack" FORCE)
    set(OPUS_DISABLE_INTRINSICS OFF CACHE BOOL "Opus: avoid architecture-specific intrinsics" FORCE)
    add_subdirectory("${OPUS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/deps/opus" EXCLUDE_FROM_ALL)
endif()

idf_component_register(SRCS "esp32-rtp.c"
                           "lifecycle_manager.c"
                           "config/config_manager.c"
                           "mdns/mdns_service.c"
                           "mdns/mdns_discovery.c"
                           "sender/network_out.c"
                           "web/web_server.c"
                           ${WEB_ROUTES_SRCS}
                           "logging/log_buffer.c"
                           "ota/ota_manager.c"
                           ${LIFECYCLE_SRCS}
                           ${RECEIVER_SRCS}
                      INCLUDE_DIRS "."
                      EMBED_FILES ${WEB_FILES})

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    target_link_libraries(${COMPONENT_LIB} PUBLIC opus)
    target_include_directories(${COMPONENT_LIB} PUBLIC "${OPUS_ROOT}/include")
    target_compile_definitions(opus PUBLIC GLOBAL_STACK_SIZE=22000)

    # Enable fixed-point math for better ESP32 performance
    target_compile_definitions(opus PUBLIC FIXED_POINT=1)

    if(IDF_TARGET STREQUAL "esp32s3")
        target_compile_definitions(opus PUBLIC OPUS_XTENSA_LX7=1)
    endif()

    # Add optimization flags for better performance
    target_compile_options(opus PRIVATE -O2)
endif()

