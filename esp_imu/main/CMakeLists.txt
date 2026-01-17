idf_component_register(SRCS "McHacks.cpp" 
			     PRIV_REQUIRES esp_adc esp_server esp_client MP6050 nvs_flash esp_driver_uart console driver bt esp_timer 
                    INCLUDE_DIRS ".")
