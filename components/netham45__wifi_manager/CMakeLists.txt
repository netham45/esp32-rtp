idf_component_register( SRCS "wifi_manager.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_wifi freertos nvs_flash esp_event)
