idf_component_register(SRCS "main.c" "rv_wifi.c"
                       REQUIRES rv_dsp rv_radio esp_hw_support
                                esp_wifi esp_netif esp_event nvs_flash)
