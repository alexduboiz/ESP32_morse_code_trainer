idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS ""
                       REQUIRES esp_wifi esp_netif esp_http_server nvs_flash esp_driver_gpio esp_timer mbedtls)
