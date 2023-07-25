#ifndef OTA
#define OTA

void simple_ota_example_task(void *pvParameter);
void reply_server(uint8_t sort, uint8_t code, const uint8_t *versions);

#endif