idf_component_register( SRCS "led_strip_controller.c" "pcm_visualizer.c" "visualizer_task.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_timer esp_ringbuf esp_driver_rmt)
