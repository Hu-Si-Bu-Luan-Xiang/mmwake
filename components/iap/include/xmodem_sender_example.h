#include "esp_xmodem.h"

#ifndef IAP
#define IAP

void start_stm32_iap(void);
esp_xmodem_handle_t stm32_serial_xmodem_iap_init(void);
void http_client_task2(void *pvParameters);

#endif