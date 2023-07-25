set(src
"xmodem_sender_example.c"
"esp_xmodem_common.c"
"esp_xmodem_sender.c"
"esp_xmodem_receiver.c"
"esp_xmodem_transport.c"
)
    
set(inc
"."
".."
"include"
"priv"
)
    
idf_component_register(SRCS ${src}
                    INCLUDE_DIRS ${inc}
                    PRIV_REQUIRES esp_http_client nvs_flash driver
                    )