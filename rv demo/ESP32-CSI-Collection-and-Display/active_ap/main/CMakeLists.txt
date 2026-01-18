idf_component_register(SRCS "main.c" "led_strip_encoder.c"
                      INCLUDE_DIRS "."
                      PRIV_REQUIRES esp_driver_rmt esp_wifi nvs_flash esp_netif lwip)