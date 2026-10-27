idf_component_register(
    SRCS "rv_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES rv_dsp esp_wifi esp_phy nvs_flash esp_netif esp_event esp_timer
             heap soc esp_hw_support)
