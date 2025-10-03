idf_component_register( SRCS "usb_out.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_ringbuf usb_host_uac usb)
