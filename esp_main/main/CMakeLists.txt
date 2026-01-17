idf_component_register(SRCS "McHacks.c" "graphics.cpp"
                       PRIV_REQUIRES esp_adc nvs_flash esp_driver_uart console driver bt esp_timer LovyanGFX
                    INCLUDE_DIRS ".")
