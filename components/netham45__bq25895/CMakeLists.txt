idf_component_register( SRCS "bq25895_integration.c" "bq25895_json.c" "bq25895.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_ringbuf esp_driver_i2c driver json)
