idf_component_register(SRCS "example_ble_server_throughput.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES bt nvs_flash esp_timer app_update)
