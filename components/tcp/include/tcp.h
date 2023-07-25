#ifndef TCP
#define TCP
void tcp_client_task(void *pvParameters);
void stay_for_stress_test_for_server(void *pvParameters);
uint8_t send_mac(uint8_t sort, const uint8_t * mac_will_send);

#endif