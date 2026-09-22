idf_component_register(SRCS "main.c" "eraseConfig.c" "http_server.c" "ble_sensors.c"
                       PRIV_REQUIRES bt esp_driver_i2c esp_driver_gpio esp_event esp_http_client esp_http_server
                       esp_netif esp_timer esp_wifi mbedtls nvs_flash network_provisioning u8g2
                       INCLUDE_DIRS ".")
