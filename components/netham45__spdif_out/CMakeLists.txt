idf_component_register( SRCS "spdif_out.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_ringbuf esp_driver_i2s driver)
