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
#include <esp_xmodem_priv.h>
#include <esp_xmodem.h>
#include <esp_xmodem_transport.h>
#include "driver/uart.h"
#include "all_config.h"
extern All_Flags all_config;
extern QueueHandle_t uart_event_queue;

static const char *TAG = "xmodem_sender";

#ifdef CONFIG_IDF_TARGET_ESP8266
#define ESP_XMODEM_SENDER_TASK_SIZE 2048
#else
#define ESP_XMODEM_SENDER_TASK_SIZE 4096
#endif
#define ESP_XMODEM_SENDER_TASK_PRIO 11

static void esp_xmodem_send_process(void *pvParameters)
{
    esp_xmodem_t *sender = (esp_xmodem_t *) pvParameters;
    esp_xmodem_config_t *p = sender->config;
    uint8_t ch = 0;
    int i = 0;

    for (i = 0; i < p->cycle_max_retry; i++ ) {
        if (esp_xmodem_read_byte(sender, &ch, p->cycle_timeout_ms) == ESP_FAIL) {
            ESP_LOGI(TAG, "Connecting to Xmodem server(%d/%d)", i + 1, p->cycle_max_retry);
            continue;
        } else {
            if (ch == CRC16 || ch == NAK) {
                sender->crc_type = (ch == CRC16) ? ESP_XMODEM_CRC16 : ESP_XMODEM_CHECKSUM;
                 /* Set state to connected */
                esp_xmodem_set_state(sender, XMODEM_STATE_CONNECTED);
                esp_xmodem_set_error_code(sender, ESP_OK);
                esp_xmodem_dispatch_event(sender, ESP_XMODEM_EVENT_CONNECTED, NULL, 0);
                break;
            } else {
                ESP_LOGE(TAG, "Receive 0x%02x data, only support 'C' or NAK", ch);
                esp_xmodem_set_error_code(sender, ESP_ERR_XMODEM_CRC_NOT_SUPPORT);
                esp_xmodem_transport_flush(sender);
                continue;
            }
        }
    }

    if (i == p->cycle_max_retry) {
        all_config.iap_start = 0;
        ESP_LOGE(TAG, "Connecting to Xmodem receiver fail");
        esp_xmodem_set_error_code(sender, ESP_ERR_NO_XMODEM_RECEIVER);
        sender->process_handle = NULL;
        esp_xmodem_dispatch_event(sender, ESP_XMODEM_EVENT_ERROR, NULL, 0);
    }
    sender->process_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp_xmodem_sender_start(esp_xmodem_handle_t sender)
{
    if (!sender) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->role != ESP_XMODEM_SENDER) {
        ESP_LOGE(TAG, "Now is sender mode,please set role to ESP_XMODEM_SENDER");
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->state != XMODEM_STATE_INIT) {
        ESP_LOGE(TAG, "sender state is not XMODEM_STATE_INIT");
        return ESP_ERR_XMODEM_STATE;
    }

    /* Set state to connecting */
    esp_xmodem_set_state(sender, XMODEM_STATE_CONNECTING);

    if (xTaskCreate(esp_xmodem_send_process, "xmodem_send", ESP_XMODEM_SENDER_TASK_SIZE, sender, ESP_XMODEM_SENDER_TASK_PRIO, &sender->process_handle) != pdPASS) {
        ESP_LOGE(TAG, "Create xmodem_sender fail");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t esp_xmodem_sender_process(esp_xmodem_t *sender, uint8_t *packet, uint32_t xmodem_packet_len, uint32_t *write_len, bool is_file)
{
    if (!sender) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_xmodem_config_t *p = sender->config;
    int i = 0;
    uint8_t ch = 0;

    for (i = 0; i < p->max_retry; i ++) {
        *write_len = esp_xmodem_send_data(sender, packet, &xmodem_packet_len);
        if (xmodem_packet_len != *write_len) {
            esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_DATA_SEND_ERROR, ESP_XMODEM_EVENT_ERROR, CAN);
            goto error;
        }
    
        if (esp_xmodem_wait_response(sender, &ch) == ESP_OK) {
            if (is_file) {
                if (ch == ACK) {
                    if (esp_xmodem_wait_response(sender, &ch) == ESP_OK) {
                        if (ch == CRC16 || ch == NAK) {
                            break;
                        } else if (ch == CAN) {
                            esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_RECEIVE_CAN, ESP_XMODEM_EVENT_ERROR, ACK);
                            goto error;
                        }
                    }
                    break;
                }
                esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_DATA_RECV_ERROR, ESP_XMODEM_EVENT_ERROR, CAN);
                goto error;
            } else {
                if (ch == ACK) {
                    break;
                } else if (ch == NAK) {
                    continue;
                } else if (ch == CAN) {
                    esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_RECEIVE_CAN, ESP_XMODEM_EVENT_ERROR, ACK);
                    goto error;
                } else {
                    esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_DATA_RECV_ERROR, ESP_XMODEM_EVENT_ERROR, CAN);
                    goto error;
                }
            }
        } else {
            ESP_LOGI(TAG, "esp_xmodem_wait_response timeout");
        }
    }

    if (i == p->max_retry) {
        esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_MAX_RETRY, ESP_XMODEM_EVENT_ERROR, CAN);
        goto error;
    }
    return ESP_OK;
error:
    return ESP_FAIL;
}

esp_err_t esp_xmodem_sender_send_file_packet(esp_xmodem_handle_t sender, char *filename, int filename_length, uint32_t total_length)
{
    uint8_t crc_len = 0;
    uint32_t write_len = 0, xmodem_packet_len = 0;
    esp_err_t ret = ESP_FAIL;
    crc_len = esp_xmodem_get_crc_len(sender);

    uint8_t *packet = sender->data;
    if (!packet) {
        ESP_LOGE(TAG, "No mem for Xmodem file packet");
        return ESP_ERR_NO_MEM;
    }
    memset(packet, 0, sender->data_len);
    packet[0] = SOH;
    packet[1] = 0x00;
    packet[2] = 0xFF;

    if (filename && filename_length && total_length) {
        memcpy(&packet[XMODEM_HEAD_LEN], filename, filename_length);
        sprintf((char *)&packet[XMODEM_HEAD_LEN + filename_length + 1], "%u%c", total_length, ' ');
        sender->is_file_data = true;
        sender->file_data.file_length = total_length;
        memcpy(sender->file_data.filename, filename, filename_length);
    } else {
        if (sender->state != XMODEM_STATE_SENDER_SEND_EOT) {
            ESP_LOGE(TAG, "Please input the filename, filename length and total length");
            return ret;
        }
    }
  
    if (crc_len == 1) {
        uint8_t checksum = esp_xmodem_checksum(&packet[XMODEM_HEAD_LEN], XMODEM_DATA_LEN);
        packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN] = checksum;
    } else {
        uint16_t crc16 = esp_xmodem_crc16(&packet[XMODEM_HEAD_LEN], XMODEM_DATA_LEN);
        packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN] = (crc16 >> 8) & 0xFF;
        packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 1] = crc16 & 0xFF;
    }

    xmodem_packet_len = XMODEM_DATA_LEN + XMODEM_HEAD_LEN + crc_len;
    if (esp_xmodem_sender_process(sender, packet, xmodem_packet_len, &write_len, true) == ESP_OK) {
        sender->pack_num = 1;
        ret = ESP_OK;
    } else {
        sender->pack_num = 0;
    }

    return ret;
}

esp_err_t send_process_wait_response(uint8_t *ch, int timeout_ms)
{
    uart_event_t event;
    uint8_t buffer[128];
    if(xQueueReceive(uart_event_queue, (void * )&event, timeout_ms/portTICK_PERIOD_MS)) {
        bzero(buffer, 128);
        switch(event.type) 
        {
            //Event of UART receving data
            case UART_DATA: 
            {
                int len = uart_read_bytes(UART_NUM_1, buffer, event.size, 50/portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "wait_response read %d bytes, event.size:%d", len, event.size);
                if (len == 1) {
                    *ch = buffer[0];
                    return ESP_OK;
                } else if(len == 2) {
                    *ch = buffer[0];
                    ESP_LOGW(TAG, "uart read 2 bytes:%d、%d", buffer[0], buffer[1]);
                    return ESP_FAIL;
                }else{
                    ESP_LOGE(TAG, "uart read %d bytes", len);
                    return ESP_FAIL;
                }
            }break;
            //Others
            default:
            {
                ESP_LOGE(TAG, "uart event type: %d", event.type);
                return ESP_FAIL;
            }break;      
        }
    } else {
        ESP_LOGE(TAG, "xQueueReceive return pdFALSE");
        return ESP_FAIL;
    }
}



esp_err_t send_file_header(char *filename, int filename_length, uint32_t total_length)
{
    uint8_t ch, packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2];
    esp_err_t ret = ESP_FAIL;

    memset(packet, 0, XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2);
    packet[0] = SOH;
    packet[1] = 0x00;
    packet[2] = 0xFF;

    if (filename && filename_length && total_length) {
        memcpy(&packet[XMODEM_HEAD_LEN], filename, filename_length);
        sprintf((char *)&packet[XMODEM_HEAD_LEN + filename_length + 1], "%u%c", total_length, ' ');
    } else {
        ESP_LOGI(TAG, "End send_file_header");
    }
    
    uint16_t crc16 = esp_xmodem_crc16(&packet[XMODEM_HEAD_LEN], XMODEM_DATA_LEN);
    packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN] = (crc16 >> 8) & 0xFF;
    packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 1] = crc16 & 0xFF;
    
    ESP_LOGI(TAG, " iap task 3 ");
    uart_flush(UART_NUM_1);
    xQueueReset(uart_event_queue);
    int res = uart_write_bytes(UART_NUM_1, packet, XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2);
    ESP_LOGI(TAG, "SOH %d;  NUM %d; ~NUM %d; CRC %d %d ", packet[0], packet[1], packet[2], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN+1]);
    if (res == XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2) {
        ESP_LOGI(TAG, "133 bytes sent success");
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "133 bytes sent failed");
        return ESP_FAIL;
    }

    if (send_process_wait_response(&ch, 5000) == ESP_OK){
        if (ch == ACK){
            ESP_LOGI(TAG, "filename sent success");
            ret = ESP_OK;
        }else{
            ESP_LOGE(TAG, "filename sent wait_response: %d", ch);
            return ESP_FAIL;
        }   
    }else{
        ESP_LOGE(TAG, "filename sent failed");
        return ESP_FAIL;
    }
    
    return ret;
}

esp_err_t xmodem_send_data_process(uint8_t *packet, uint32_t xmodem_packet_len, uint32_t *write_len)
{
    int i = 0, max_retry = 10;
    uint8_t ch = 0;

    for (i = 0; i < max_retry; i++) {

        // ESP_LOGI(TAG, "pack num:%d ; send process: %d/%d", packet[1], i+1, max_retry);
        *write_len = uart_write_bytes(UART_NUM_1, packet, xmodem_packet_len);
        if (xmodem_packet_len != *write_len) {
            ESP_LOGE(TAG, "xmodem_packet_len != *write_len");
            goto error;
        }
        uart_flush(UART_NUM_1);
        xQueueReset(uart_event_queue);
        if (send_process_wait_response(&ch, 500) == ESP_OK) {
            
            if (ch == ACK) {
                ESP_LOGI(TAG, "Receive ACK, send success");
                break;
            } else if (ch == NAK) {
                ESP_LOGW(TAG, "Receive NAK");
                continue;
            } else if (ch == CAN) {
                ESP_LOGE(TAG, "Receive CAN");
                goto error;
            } else {
                ESP_LOGE(TAG, "Receive unexpectedly: %d", ch);
                continue;
            }
            
        } else {
            ESP_LOGW(TAG, "Send data process wait_response timeout or failed.");
        }
    }

    if (i == max_retry) {
        ESP_LOGE(TAG, "send failed : process reach max retry!");
        goto error;
    }
    return ESP_OK;
error:
    return ESP_FAIL;
}

esp_err_t xmodem_send_data(esp_xmodem_handle_t sender, uint8_t *data, uint32_t len)
{

    if (!sender) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->role != ESP_XMODEM_SENDER) {
        ESP_LOGE(TAG, "Now is sender mode, please set role to ESP_XMODEM_SENDER");
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->state != XMODEM_STATE_CONNECTED) {
        ESP_LOGE(TAG, "sender state is not XMODEM_STATE_CONNECTED");
        return ESP_ERR_XMODEM_STATE;
    }

    if (!data) {
        ESP_LOGE(TAG, "The send data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t crc_len = 2, head = SOH, *packet = sender->data;
    uint32_t write_len = 0, xmodem_data_len = 0, xmodem_packet_len = 0, need_write_len = 0;
    int32_t left_len = len;
    esp_err_t ret = ESP_FAIL;
    // esp_xmodem_config_t *p = sender->config;

    while (left_len > 0) {
        if (left_len <= XMODEM_DATA_LEN) {
            head = SOH;
            xmodem_data_len = XMODEM_DATA_LEN;
        } else {
            head = STX;
            xmodem_data_len = XMODEM_1K_DATA_LEN;
        }
        
        memset(packet, 0, XMODEM_1K_DATA_LEN + XMODEM_HEAD_LEN + 2);
        
        need_write_len = (left_len > xmodem_data_len) ? xmodem_data_len : left_len;
        packet[0] = head;
        packet[1] = sender->pack_num & 0xFF;
        packet[2] = ~(sender->pack_num & 0xFF);

        memcpy(&packet[XMODEM_HEAD_LEN], data + write_len, need_write_len);
        
        uint16_t crc16 = esp_xmodem_crc16(&packet[XMODEM_HEAD_LEN], xmodem_data_len);
        packet[xmodem_data_len + XMODEM_HEAD_LEN] = (crc16 >> 8) & 0xFF;
        packet[xmodem_data_len + XMODEM_HEAD_LEN + 1] = crc16 & 0xFF;
        
        ESP_LOGI(TAG, "packet: SOH %d; NUM %d ~NUM %d; pack[0]:%d pakck[-1]%d; CRC %d %d ", packet[0], packet[1], packet[2], packet[XMODEM_HEAD_LEN], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN-1], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN+1]);

        xmodem_packet_len = XMODEM_HEAD_LEN + xmodem_data_len + crc_len;
        if (xmodem_send_data_process(packet, xmodem_packet_len, &write_len) == ESP_OK) {
            left_len -= need_write_len;
            sender->write_len += need_write_len;
            sender->pack_num ++;
            ret = ESP_OK;
        } else {
            sender->pack_num = 1;
            ret = ESP_FAIL;
            goto err;
        }
    }
err:
    return ret;
}

esp_err_t xmodem_send_cancel(void)
{
    uint8_t ch= CAN;
    // 中断发送进程

    uart_flush(UART_NUM_1);
    xQueueReset(uart_event_queue);

    int res = uart_write_bytes(UART_NUM_1, &ch, 1);
    if (res != 1){
        ESP_LOGE(TAG, "Send CAN 1 failed: res != 1");
        return ESP_FAIL;
    }
    res = uart_write_bytes(UART_NUM_1, &ch, 1);
    if (res != 1){
        ESP_LOGE(TAG, "Send CAN 2 failed: res != 1");
        return ESP_FAIL;
    }

    if(send_process_wait_response(&ch, 1000) == ESP_OK){
        if(ch == ACK){
            ESP_LOGI(TAG, "Send CAN success");
            return ESP_OK;
        }else{
            ESP_LOGE(TAG, "Send CAN , ch != ACK");
        }
        
    } else {
        ESP_LOGE(TAG, "Send CAN failed");
        return ESP_FAIL;
    }

    return ESP_OK;

}

esp_err_t xmodem_send_eot(void)
{
    uint8_t ch= EOT;
    // 结束发送

    uart_flush(UART_NUM_1);
    xQueueReset(uart_event_queue);

    int res = uart_write_bytes(UART_NUM_1, &ch, 1);
    if (res != 1){
        ESP_LOGE(TAG, "Send EOT failed:res != 1");
        return ESP_FAIL;
    }

    if(send_process_wait_response(&ch, 1000) == ESP_OK){
        if(ch == ACK){
            ESP_LOGI(TAG, "Send EOT success");
            return ESP_OK;
        }else{
            ESP_LOGE(TAG, "Send EOT , ch != ACK");
        }
        
    } else {
        ESP_LOGE(TAG, "Send EOT failed");
        return ESP_FAIL;
    }

    return ESP_OK;

}

esp_err_t send_file_header_to_quit(void)
{
    uint8_t ch, packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2];
    esp_err_t ret = ESP_FAIL;

    memset(packet, 0, XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2);
    packet[0] = SOH;
    packet[1] = 0x00;
    packet[2] = 0xFF;
    
    uint16_t crc16 = esp_xmodem_crc16(&packet[XMODEM_HEAD_LEN], XMODEM_DATA_LEN);
    packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN] = (crc16 >> 8) & 0xFF;
    packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 1] = crc16 & 0xFF;
    
    ESP_LOGI(TAG, " iap task end ");
    uart_flush(UART_NUM_1);
    xQueueReset(uart_event_queue);
    int res = uart_write_bytes(UART_NUM_1, packet, XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2);
    ESP_LOGI(TAG, "SOH %d;  NUM %d; ~NUM %d; CRC %d %d ", packet[0], packet[1], packet[2], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN], packet[XMODEM_DATA_LEN + XMODEM_HEAD_LEN+1]);
    if (res == XMODEM_DATA_LEN + XMODEM_HEAD_LEN + 2) {
        ESP_LOGI(TAG, "End 133 bytes sent success");
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "End 133 bytes sent failed");
        return ESP_FAIL;
    }

    if (send_process_wait_response(&ch, 2000) == ESP_OK){
        if (ch == ACK){
            ESP_LOGI(TAG, "session end sent success");
            ret = ESP_OK;
        }else{
            ESP_LOGE(TAG, "session end sent wait_response: %d", ch);
            return ESP_FAIL;
        }   
    }else{
        ESP_LOGE(TAG, "session end sent failed");
        return ESP_FAIL;
    }
    
    return ret;
}

uint8_t send_order_to_stm32(uint8_t order)
{
    uint8_t ch = order;
    
    uart_flush(UART_NUM_1);
    xQueueReset(uart_event_queue);

    int res = uart_write_bytes(UART_NUM_1, &ch, 1);
    if (res != 1){
        ESP_LOGE(TAG, "Send order %c failed:res != 1", ch);
        return STM_DISCONNECT;
    }
    
    if(send_process_wait_response(&ch, 1000) == ESP_OK){   
        return ch;
    } 
    
    ESP_LOGE(TAG, "Send order receive failed!");
    return STM_DISCONNECT;
}

// uint8_t get_stm32_mode(void)
// {
//     uint8_t ch = CHECK_MODE;

//     int res = uart_write_bytes(UART_NUM_1, &ch, 1);
//     if (res != 1){
//         ESP_LOGE(TAG, "Send CHECK_MODE failed:res != 1");
//         return STM_DISCONNECT;
//     }

//     uart_flush(UART_NUM_1);
//     xQueueReset(uart_event_queue);

//     if(send_process_wait_response(&ch, 1000) == ESP_OK){
//         if(ch == MODE_A || ch == MODE_B || ch == MODE_C || ch == MODE_D){
//             ESP_LOGI(TAG, "STM32 running mode is :%c", ch);
//             return ch;
//         }else{
//             ESP_LOGE(TAG, "Error receive.");
//             return STM_DISCONNECT;
//         }
//     } else {
//         ESP_LOGE(TAG, "CHECK_MODE receive nothing.");
//         return STM_DISCONNECT;
//     }
    
//     return STM_DISCONNECT;
// }


// esp_err_t reset_stm32(void)
// {
//     uint8_t ch = REBOOT;
    
//     int res = uart_write_bytes(UART_NUM_1, &ch, 1);
//     if (res != 1){
//         ESP_LOGE(TAG, "Send REBOOT failed:res != 1");
//         return ESP_FAIL;
//     }

//     uart_flush(UART_NUM_1);
//     xQueueReset(uart_event_queue);

//     if(send_process_wait_response(&ch, 1000) == ESP_OK){
//         if(ch == REBOOT){
//             ESP_LOGI(TAG, "STM32 reboot success.");
//             return ESP_OK;
//         }
//     } 
    
//     ESP_LOGE(TAG, "Reset failed!");
//     return ESP_FAIL;
// }

// esp_err_t send_upgrade_order(void)
// {
//     uint8_t ch = UPGRADE;
    
//     int res = uart_write_bytes(UART_NUM_1, &ch, 1);
//     if (res != 1){
//         ESP_LOGE(TAG, "Send UPGRADE failed:res != 1");
//         return ESP_FAIL;
//     }

//     uart_flush(UART_NUM_1);
//     xQueueReset(uart_event_queue);

//     if(send_process_wait_response(&ch, 1000) == ESP_OK){
//         if(ch == MODE_D){
//             ESP_LOGI(TAG, "STM32 reboot success.");
//             return ESP_OK;
//         }
//     } 
    
//     ESP_LOGE(TAG, "Reset failed!");
//     return ESP_FAIL;

// }

esp_err_t esp_xmodem_sender_send(esp_xmodem_handle_t sender, uint8_t *data, uint32_t len)
{
    if (!sender) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->role != ESP_XMODEM_SENDER) {
        ESP_LOGE(TAG, "Now is sender mode, please set role to ESP_XMODEM_SENDER");
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->state != XMODEM_STATE_CONNECTED) {
        ESP_LOGE(TAG, "sender state is not XMODEM_STATE_CONNECTED");
        return ESP_ERR_XMODEM_STATE;
    }

    if (!data) {
        ESP_LOGE(TAG, "The send data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t crc_len = 0, head = SOH, *packet = NULL;
    uint32_t write_len = 0, xmodem_data_len = 0, xmodem_packet_len = 0, need_write_len = 0;
    int32_t left_len = len;
    esp_err_t ret = ESP_FAIL;
    crc_len = esp_xmodem_get_crc_len(sender);
    esp_xmodem_config_t *p = sender->config;

    while (left_len > 0) {
        if (p->support_xmodem_1k) {
            if (left_len <= XMODEM_DATA_LEN) {
                head = SOH;
                xmodem_data_len = XMODEM_DATA_LEN;
            } else {
                head = STX;
                xmodem_data_len = XMODEM_1K_DATA_LEN;
            }
        } else {
            head = SOH;
            xmodem_data_len = XMODEM_DATA_LEN;
        }
        packet = sender->data;
        if (!packet) {
            ESP_LOGE(TAG, "No mem for Xmodem packet");
            return ESP_ERR_NO_MEM;
        }
        memset(packet, 0, sender->data_len);
        if (sender->is_file_data) {
            memset(&packet[XMODEM_HEAD_LEN], 0x1A, xmodem_data_len);
        }
        need_write_len = (left_len > xmodem_data_len) ? xmodem_data_len : left_len;
        packet[0] = head;
        packet[1] = sender->pack_num & 0xFF;
        packet[2] = ~(sender->pack_num & 0xFF);

        memcpy(&packet[XMODEM_HEAD_LEN], data + write_len, need_write_len);
        if (crc_len == 1) {
            uint8_t checksum = esp_xmodem_checksum(&packet[XMODEM_HEAD_LEN], xmodem_data_len);
            packet[xmodem_data_len + XMODEM_HEAD_LEN] = checksum;
        } else {
            uint16_t crc16 = esp_xmodem_crc16(&packet[XMODEM_HEAD_LEN], xmodem_data_len);
            packet[xmodem_data_len + XMODEM_HEAD_LEN] = (crc16 >> 8) & 0xFF;
            packet[xmodem_data_len + XMODEM_HEAD_LEN + 1] = crc16 & 0xFF;
        }

        xmodem_packet_len = XMODEM_HEAD_LEN + xmodem_data_len + crc_len;
        if (esp_xmodem_sender_process(sender, packet, xmodem_packet_len, &write_len, false) == ESP_OK) {
            left_len -= need_write_len;
            sender->write_len += need_write_len;
            sender->pack_num ++;
            ret = ESP_OK;
        } else {
            sender->pack_num = 1;
            ret = ESP_FAIL;
            goto err;
        }
    }
err:
    return ret;
}

esp_err_t esp_xmodem_sender_send_cancel(esp_xmodem_handle_t sender)
{
    if (!sender) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->role != ESP_XMODEM_SENDER) {
        ESP_LOGE(TAG, "Now is sender mode, please set role to ESP_XMODEM_SENDER");
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->state != XMODEM_STATE_CONNECTED) {
        ESP_LOGE(TAG, "sender state is not XMODEM_STATE_CONNECTED");
        return ESP_ERR_XMODEM_STATE;
    }
    esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_BASE, ESP_XMODEM_EVENT_INIT, CAN);
    return ESP_OK;
}


esp_err_t esp_xmodem_sender_send_eot(esp_xmodem_handle_t sender)
{
    if (!sender) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->role != ESP_XMODEM_SENDER) {
        ESP_LOGE(TAG, "Now is sender mode, please set role to ESP_XMODEM_SENDER");
        return ESP_ERR_INVALID_ARG;
    }

    if (sender->state != XMODEM_STATE_CONNECTED) {
        ESP_LOGE(TAG, "sender state is not XMODEM_STATE_CONNECTED");
        return ESP_ERR_XMODEM_STATE;
    }

    int i = 0;
    uint8_t ch = 0;
    esp_xmodem_config_t *p = sender->config;

    for (i = 0; i < p->max_retry; i ++) {
        if (esp_xmodem_get_state(sender) != XMODEM_STATE_SENDER_SEND_EOT) {
            esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_BASE, ESP_XMODEM_EVENT_INIT, EOT);
        }
        if (esp_xmodem_read_byte(sender, &ch, p->cycle_timeout_ms) == ESP_OK) {
            if (esp_xmodem_get_state(sender) == XMODEM_STATE_SENDER_SEND_EOT) {
                if (ch == NAK) {
                    sender->crc_type = ESP_XMODEM_CHECKSUM;
                } else if (ch == CRC16) {
                    sender->crc_type = ESP_XMODEM_CRC16;
                } else {
                    return ESP_OK;
                }
                return esp_xmodem_sender_send_file_packet(sender, NULL, 0, 0);
            } else {
                if (ch == ACK) {
                    esp_xmodem_set_state(sender, XMODEM_STATE_SENDER_SEND_EOT);
                }
            }
        } else {
            if (esp_xmodem_get_state(sender) == XMODEM_STATE_SENDER_SEND_EOT) {
                break;
            }
        }
    }

    if (i == p->max_retry) {
        esp_xmodem_send_char_code_event(sender, ESP_ERR_XMODEM_MAX_RETRY, ESP_XMODEM_EVENT_ERROR, CAN);
        return ESP_FAIL;
    } else {
        return ESP_OK;
    }
}
