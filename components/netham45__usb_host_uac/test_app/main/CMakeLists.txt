include($ENV{IDF_PATH}/tools/cmake/version.cmake)

idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       REQUIRES unity usb usb_host_uac
                       EMBED_FILES new_epic.wav)

# force-link test_host_uac.c
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-u test_uac_setup")
