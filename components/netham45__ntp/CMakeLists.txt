idf_component_register( SRCS "ntp_client.cpp"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES freertos esp_netif mdns lwip)
