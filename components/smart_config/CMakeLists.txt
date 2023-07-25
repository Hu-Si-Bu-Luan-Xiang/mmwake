idf_component_register(SRCS "smart_config.c"
                    INCLUDE_DIRS "include" ".."
                    REQUIRES esp_wifi nvs_flash wpa_supplicant)
