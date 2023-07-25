// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "esp_xmodem_transport.h"
#include "esp_xmodem.h"
#include <esp_xmodem_priv.h>

static const char *TAG = "xmodem_transport";

extern QueueHandle_t *uart_event_queue_handle;
extern QueueHandle_t uart_event_queue;

#define ESP_XMODEM_TRANSPORT_UART_RECV_BUF_SIZE  128
#define ESP_XMODEM_TRANSPORT_TASK_SIZE 2048
#define ESP_XMODEM_TRANSPORT_TASK_PRIO 12
#define ESP_XMODEM_TRANSPORT_CHECK(handle, message) do { if (!(handle)) { \
    ESP_LOGE(TAG, message); return ESP_FAIL;}} while(0)


extern QueueHandle_t *uart_event_queue_handle;
extern QueueHandle_t uart_event_queue;

/* Send data to UART */
int esp_xmodem_send_data(esp_xmodem_t *handle, uint8_t *data, uint32_t *len)
{
    uint32_t send_len = *len;
    esp_xmodem_print_packet(data, send_len);
    ESP_XMODEM_TRANSPORT_CHECK(handle, "handle is NULL");
    esp_xmodem_config_t *p = handle->config;
    if (p->user_data_size) {
        esp_xmodem_dispatch_event(handle, ESP_XMODEM_EVENT_ON_SEND_DATA, data, send_len);
        if (handle->event.data && handle->event.data_len) {
            *len = send_len = handle->event.data_len;
        } else {
            return ESP_FAIL;
        }
    }
    return uart_write_bytes(handle->transport->uart_num, (char *) data, send_len);
}

/* Read A Byte data from UART */
esp_err_t esp_xmodem_read_byte(esp_xmodem_t *handle, uint8_t *ch, int timeout_ms)
{
    ESP_XMODEM_TRANSPORT_CHECK(handle, "handle is NULL");
    uint32_t len = esp_xmodem_read_data(handle, timeout_ms);
    if (len > 0) {
        *ch = (*(uint8_t *)handle->data);
        return ESP_OK;
    }

    return ESP_FAIL;
}

/* Read data from UART */
uint32_t esp_xmodem_read_data(esp_xmodem_t *handle, int timeout_ms)
{
    ESP_XMODEM_TRANSPORT_CHECK(handle, "handle is NULL");
    esp_xmodem_config_t *p = handle->config;
    uart_event_t event;
    size_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    size_t start_ticks = xTaskGetTickCount();
    uint32_t expired_ticks = 0, read_len = 0;
    uint8_t buffer[ESP_XMODEM_TRANSPORT_UART_RECV_BUF_SIZE];
    uint8_t *packet = handle->data;

    for (;;) {
        expired_ticks = xTaskGetTickCount() - start_ticks;
        if (timeout_ticks <= expired_ticks) {
            goto err;
        }
        if(xQueueReceive(handle->transport->uart_queue, (void * )&event, handle->transport->recv_timeout/portTICK_PERIOD_MS)) {
            bzero(buffer, ESP_XMODEM_TRANSPORT_UART_RECV_BUF_SIZE);
            switch(event.type) {
                //Event of UART receving data
                case UART_DATA: {
                    if (read_len == 0) {
                        memset(packet, 0, handle->data_len);
                    }
                    ESP_LOGD(TAG, "[UART DATA]: %d", event.size);
                    expired_ticks = xTaskGetTickCount() - start_ticks;
                    if (timeout_ticks > expired_ticks) {
                        int len = uart_read_bytes(handle->transport->uart_num, buffer, event.size, timeout_ticks - expired_ticks);
                        if (len > 0) {
                            memcpy(&packet[read_len], buffer, len);
                            read_len += len;
                        }
                        if (len < 120) { //regard it all data has received
                            if (p->user_data_size) {
                                esp_xmodem_dispatch_event(handle, ESP_XMODEM_EVENT_ON_RECEIVE_DATA, packet, read_len);
                                if (handle->event.data && handle->event.data_len) {
                                    read_len = handle->event.data_len;
                                    return read_len;
                                }
                            } else {
                                return read_len;
                            }
                        }
                        continue;
                    }
                    goto err;
                }
                //Event of HW FIFO overflow detected
                case UART_FIFO_OVF:
                    ESP_LOGE(TAG, "hw fifo overflow\n");
                    uart_flush(handle->transport->uart_num);
                    xQueueReset(handle->transport->uart_queue);
                    continue;
                //Event of UART ring buffer full
                case UART_BUFFER_FULL:
                    ESP_LOGE(TAG, "ring buffer full\n");
                    uart_flush(handle->transport->uart_num);
                    xQueueReset(handle->transport->uart_queue);
                    continue;
                //Event of UART parity check error
                case UART_PARITY_ERR:
                    ESP_LOGE(TAG, "uart parity error\n");
                    continue;
                //Event of UART frame error
                case UART_FRAME_ERR:
                    ESP_LOGE(TAG, "uart frame error\n");
                    continue;
                //Others
                default:
                    ESP_LOGI(TAG, "uart event type: %d\n", event.type);
                    continue;
            }
        } else {
            if (read_len > 0) {
                if (p->user_data_size) {
                    esp_xmodem_dispatch_event(handle, ESP_XMODEM_EVENT_ON_RECEIVE_DATA, packet, read_len);
                    if (handle->event.data && handle->event.data_len) {
                        read_len = handle->event.data_len;
                        return read_len;
                    }
                } else {
                    return read_len;
                }
            }
        }
    }
err:
    return 0;
}

esp_err_t esp_xmodem_transport_close(esp_xmodem_handle_t handle)
{
    if (handle) {
        esp_xmodem_transport_t *p = (esp_xmodem_transport_t *)(handle->transport);
        if (p->uart_task_handle) {
            vTaskDelete(p->uart_task_handle);
            p->uart_task_handle = NULL;
        }
        if (p->uart_queue) {
            ESP_ERROR_CHECK(uart_driver_delete(p->uart_num));
            xQueueReset(p->uart_queue);
            p->uart_queue = NULL;
        }
    }
    return ESP_OK;
}

void esp_xmodem_transport_flush(esp_xmodem_t *handle)
{
    if (handle && handle->transport->uart_queue) {
        uart_flush(handle->transport->uart_num);
        xQueueReset(handle->transport->uart_queue);
    }
}

static void esp_xmodem_transport_set_config(const esp_xmodem_transport_config_t *config, esp_xmodem_transport_config_t *init_config)
{
    init_config->uart_num = config->uart_num;
    init_config->baund_rate = !config->baund_rate ? 115200 : config->baund_rate;
    init_config->rx_buffer_size = !config->rx_buffer_size ? 2048 : config->rx_buffer_size;
    init_config->tx_buffer_size = !config->tx_buffer_size ? 2048 : config->tx_buffer_size;
    init_config->queue_size = !config->queue_size ? 10 : config->queue_size;
    init_config->recv_timeout = !config->recv_timeout ? 100 : config->recv_timeout;
    init_config->swap_pin = !config->swap_pin ? false : config->swap_pin;
#ifndef CONFIG_IDF_TARGET_ESP8266
    init_config->tx_pin = config->tx_pin;
    init_config->rx_pin = config->rx_pin;
    init_config->cts_pin = config->cts_pin;
    init_config->rts_pin = config->rts_pin;
#endif
}

esp_xmodem_transport_handle_t esp_xmodem_transport_init(esp_xmodem_transport_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Xmodem transport config is NULL");
        return NULL;
    }
    esp_xmodem_transport_config_t init_config = {0};
    esp_xmodem_transport_t *p = calloc(1, sizeof(esp_xmodem_transport_t));

    if (!p) {
        ESP_LOGE(TAG, "No mem for transport config");
        return NULL;
    }

    esp_xmodem_transport_set_config(config, &init_config);
    p->uart_num = UART_NUM_1;
    p->recv_timeout = init_config.recv_timeout;
    // Initialize the Uart task, it's used to receive and send Xmodem data.
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

#ifndef CONFIG_IDF_TARGET_ESP8266
    if (init_config.swap_pin) {
        int32_t tx_pin = init_config.tx_pin;
        int32_t rx_pin = init_config.rx_pin;
        int32_t cts_pin = init_config.cts_pin;
        int32_t rts_pin = init_config.rts_pin;
        uart_set_pin(init_config.uart_num, tx_pin, rx_pin, rts_pin, cts_pin);
    } else {
        uart_set_pin(init_config.uart_num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
#endif

    uart_event_queue = xQueueCreate(10, sizeof(uart_event_t));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 2048, 2048, 10, &uart_event_queue, 0));
    // 配置参数赋予
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    // 设置tx rx 流控位脚
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 19, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // uart_event_queue = xQueueCreate(10, sizeof(uart_event_t));
    // ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, init_config.rx_buffer_size, init_config.tx_buffer_size, init_config.queue_size, uart_event_queue, 0));
    // // 配置参数赋予
    // ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    // // 设置tx rx 流控位脚
    // ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 19, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "串口初始化成功-串口为TX: IO19 | RX: IO18");

    // ESP_ERROR_CHECK(uart_param_config(init_config.uart_num, &uart_config));
    // ESP_ERROR_CHECK(uart_driver_install(init_config.uart_num, init_config.rx_buffer_size, init_config.tx_buffer_size, init_config.queue_size, &p->uart_queue, 0));
    // ESP_ERROR_CHECK(uart_set_pin(init_config.uart_num, init_config.tx_pin, init_config.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGW(TAG, "1 uart_num:%d, tx_pin:%d, rx_pin:%d, rx_size:%d, tx_size:%d, queue_size%d ", init_config.uart_num, init_config.tx_pin, init_config.rx_pin, init_config.rx_buffer_size, init_config.tx_buffer_size, init_config.queue_size);
    // uart_event_queue_handle = &uart_event_queue;


#ifdef CONFIG_IDF_TARGET_ESP8266
    if (init_config.swap_pin) {
        uart_enable_swap();
    }
#endif
    return p;
}
