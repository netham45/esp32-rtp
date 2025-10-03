idf_component_register( SRCS "usb_in.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_ringbuf usb_device_uac esp_timer)
