idf_component_register(SRCS "McHacks.cpp" "graphics.cpp" "sd_card.cpp" "speaker.cpp"
			     PRIV_REQUIRES esp_adc esp_server esp_client MP6050 nvs_flash esp_driver_uart console driver bt esp_timer LovyanGFX fatfs vfs sdmmc esp_driver_sdspi esp_driver_i2s
                    INCLUDE_DIRS ".")
