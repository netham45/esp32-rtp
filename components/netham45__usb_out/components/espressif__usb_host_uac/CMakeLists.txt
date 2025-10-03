idf_component_register( SRCS "uac_quirks.c" "uac_descriptors.c" "uac_host.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES usb esp_ringbuf)
