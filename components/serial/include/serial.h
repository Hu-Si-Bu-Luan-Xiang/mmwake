#ifndef SERIAL
#define SERIAL
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "driver/gpio.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
// #include "protocol_examples_common.h"
#include "esp_sntp.h"

void rx_task(void *arg);
void serial_init(void);
uint8_t cood_data(const uint8_t * data,uint8_t *target,const uint8_t * save_bit,const uint8_t sort);

//tcp发送的buf
extern uint8_t* tcp_send_buf;
extern int tcp_bag_len;

#endif
