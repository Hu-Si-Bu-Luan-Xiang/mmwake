/* Xmodem Sender Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_event.h"
// #include "protocol_examples_common.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "driver/uart.h"
#include "esp_xmodem.h"
#include "esp_xmodem_transport.h"
#include "priv/esp_xmodem_priv.h"
#include "all_config.h"
#include "ota/include/simple_ota_example.h"

// #define WEB_PORT "8072"
// #define WEB_IP "8:134:111:197"
#define FILE_NAME "SleepMonitoringApp_1_0_1.bin"
#define STM32_APP_UPGRADE_URL "http://8.134.111.197:8072/SleepMonitoringApp_1_0_1.bin"
#define OTA_BUF_SIZE 1024
#define CONFIG_SUPPORT_FILE   // 定义这个即支持Ymodem协议

extern QueueHandle_t *uart_event_queue_handle;
extern QueueHandle_t uart_event_queue;
extern SemaphoreHandle_t MuxSem_TCP_Handle;
extern esp_xmodem_handle_t myXmodemHandle;
extern esp_xmodem_t myXmodem;
extern All_Flags all_config;
extern uint8_t stm32Versions[3];          // STM32程序版本号
extern uint8_t iap_reply;
extern int success_times;
extern int time_start;
extern int time_end;
extern int average_time;
extern int sum_time;
static const char *TAG = "xmodem_sender_example";

static char upgrade_data_buf[OTA_BUF_SIZE + 1];
// static uint8_t send_data_buf[XMODEM_1K_DATA_LEN + XMODEM_HEAD_LEN + 2];

static void http_client_task(void *pvParameters)
{
    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
    esp_xmodem_handle_t xmodem_sender = (esp_xmodem_handle_t) pvParameters;
    esp_err_t err;
    esp_http_client_config_t config = {
        .url = STM32_APP_UPGRADE_URL,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (http_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        goto err;
    }
    err = esp_http_client_open(http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        goto err;
    }
    int content_length = esp_http_client_fetch_headers(http_client);

    if (content_length <= 0) {
        ESP_LOGE(TAG, "No content found in the image");
        goto FAIL;
    }

    int image_data_remaining = content_length;
    int binary_file_len = 0;

#ifdef CONFIG_SUPPORT_FILE
    if (esp_xmodem_sender_send_file_packet(xmodem_sender, FILE_NAME, strlen(FILE_NAME), content_length) != ESP_OK) {
        ESP_LOGE(TAG, "Send filename fail");
        goto FAIL;
    }
#endif
    while (image_data_remaining != 0) {
        int data_max_len;
        if (image_data_remaining < OTA_BUF_SIZE) {
            data_max_len = image_data_remaining;
        } else {
            data_max_len = OTA_BUF_SIZE;
        }
        int data_read = esp_http_client_read(http_client, upgrade_data_buf, data_max_len);
        if (data_read == 0) {
            if (errno == ECONNRESET || errno == ENOTCONN) {
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                break;
            }
            if (content_length == binary_file_len) {
                ESP_LOGI(TAG, "Connection closed,all data received");
                break;
            }
        } else if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            break;
        } else if (data_read > 0) {
            err = esp_xmodem_sender_send(xmodem_sender, (uint8_t *)upgrade_data_buf, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error:esp_xmodem_sender_send failed!");
                goto FAIL;
            }
            image_data_remaining -= data_read;
            binary_file_len += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_len);
        }
    }
    ESP_LOGD(TAG, "Total binary data length writen: %d", binary_file_len);

    if (content_length != binary_file_len) {
        ESP_LOGE(TAG, "Error in receiving complete file");
        esp_xmodem_sender_send_cancel(xmodem_sender);
        goto FAIL;
    } else {
        err = esp_xmodem_sender_send_eot(xmodem_sender);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error:esp_xmodem_sender_send_eot FAIL!");
            goto FAIL;
        }
        ESP_LOGI(TAG, "Send image success");
        esp_xmodem_transport_close(xmodem_sender);
        esp_xmodem_clean(xmodem_sender);
    }
FAIL:
    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);
err:
    xSemaphoreGive(MuxSem_TCP_Handle); 
    vTaskDelete(NULL);
}

void init_myxmodem(esp_xmodem_handle_t xmodem_sender)
{
    xmodem_sender->state = XMODEM_STATE_CONNECTED;
    xmodem_sender->role = ESP_XMODEM_SENDER;
    xmodem_sender->pack_num = 1;
    xmodem_sender->write_len = 0;
}

esp_http_client_handle_t http_reconnect(esp_http_client_handle_t http_client, int start_byte)
{
    esp_http_client_config_t config = {
        .url = STM32_APP_UPGRADE_URL,
    };

    if(http_client != NULL) {  // 先关闭之前那个http连接
        esp_http_client_cleanup(http_client);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
 
    esp_http_client_handle_t new_client = esp_http_client_init(&config);  // 重新初始化新建一个http_client

    if (new_client == NULL) {
        ESP_LOGE(TAG, "Failed to re_initialise HTTP connection");
        return NULL;
    }

    esp_err_t err = esp_http_client_open(new_client, 0);  // 重新打开http_client连接，发送http请求头，形参write_len为发送内容数据长度,设置为0忽略这个参数
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re_open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(new_client);
        return NULL;
    }

    char range_header[32];
    snprintf(range_header, 32, "bytes=%d-\r\n", start_byte);
    esp_http_client_set_header(new_client, "Range", range_header);  // 设置http请求头

    
    // int content_length = esp_http_client_fetch_headers(new_client);  // 读取数据，自动处理掉响应头并返回接收包长度

    // if (content_length <= 0) {
    //     ESP_LOGE(TAG, "No content found in the image");
    // } else {
    //     ESP_LOGI(TAG, "Reconnect Image length: %d", content_length);
    // }

    return new_client;
}

void http_client_task2(void *pvParameters)
{
    esp_xmodem_handle_t xmodem_sender = (esp_xmodem_handle_t) pvParameters;
    esp_err_t err;
    esp_http_client_config_t config = {
        .url = STM32_APP_UPGRADE_URL,
    };

    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    xSemaphoreGive(MuxSem_TCP_Handle);
    if (http_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        goto err;
    }
    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
    err = esp_http_client_open(http_client, 0);
    xSemaphoreGive(MuxSem_TCP_Handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        goto err;
    }
    int content_length = -1;
    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
    content_length = esp_http_client_fetch_headers(http_client);
    xSemaphoreGive(MuxSem_TCP_Handle);
    int binary_file_len = 0;
    if (content_length <= 0) {
        ESP_LOGE(TAG, "No content found in the image");
        goto FAIL;
    } else {
        ESP_LOGI(TAG, "Image length: %d", content_length);
    }

    int image_data_remaining = content_length;
    
    int data_read = 0;
    int retry_times = 3;
    uint8_t last_process = 0;

    init_myxmodem(xmodem_sender);

    // 发送文件名和文件大小
    if (send_file_header(FILE_NAME, strlen(FILE_NAME), content_length) != ESP_OK) {
        ESP_LOGE(TAG, "Send filename fail");
        goto FAIL;
    }
    

    while (image_data_remaining != 0 && retry_times > 0) 
    {
        int data_max_len;
        if (image_data_remaining < OTA_BUF_SIZE) {
            data_max_len = image_data_remaining;
        } else {
            data_max_len = OTA_BUF_SIZE;
        }
        
        if (http_client){
            xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
            data_read = esp_http_client_read(http_client, upgrade_data_buf, data_max_len);
            xSemaphoreGive(MuxSem_TCP_Handle);
        }else{
            ESP_LOGE(TAG, "http_client is null!");
            xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
            http_client = http_reconnect(http_client, binary_file_len);
            xSemaphoreGive(MuxSem_TCP_Handle);
            retry_times--;
            continue;
        }
       
        if (data_read == 0) {
            if (content_length == binary_file_len) {
                ESP_LOGI(TAG, "All data received.");
                break;
            }else{
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                if(retry_times > 0){
                    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
                    http_client = http_reconnect(http_client, binary_file_len);
                    xSemaphoreGive(MuxSem_TCP_Handle);
                    retry_times--;
                    continue;
                }else{
                    break;
                }
            }
        } else if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            if(retry_times > 0){
                xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
                http_client = http_reconnect(http_client, binary_file_len);
                xSemaphoreGive(MuxSem_TCP_Handle);
                retry_times--;
                continue;
            }else{
                break;
            }
        } else if (data_read > 0) {
            retry_times = 3;
            err = xmodem_send_data(xmodem_sender, (uint8_t *)upgrade_data_buf, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error:esp_xmodem_sender_send failed!");
                goto FAIL;
            }
            image_data_remaining -= data_read;
            binary_file_len += data_read;
            ESP_LOGI(TAG, "Written image length %d", binary_file_len);
            uint8_t process = (uint8_t)(binary_file_len * 10 / content_length);
            if (last_process != process && process < 10) {
                reply_server(0x0d, (uint8_t)(process+10), stm32Versions);
                ESP_LOGW(TAG, "STM32 Image download process: %d / %d --- %0.1f%%", binary_file_len, content_length, (float)binary_file_len * 100 / content_length); // 下载进度(百分比)
                last_process = process;
                
            }
        }
        vTaskDelay(400 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "Image length: %d", content_length);
    ESP_LOGI(TAG, "Total binary data length writen: %d", binary_file_len);
    
    if (content_length != binary_file_len) {
        // 中断发送进程
        ESP_LOGE(TAG, "Error in receiving complete file");
        xmodem_send_cancel();
        goto FAIL;
    } else {
        // 完成，发送结束标志
        err = xmodem_send_eot();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error:esp_xmodem_sender_send_eot FAIL!");
            goto FAIL;
        }
        err = send_file_header_to_quit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error:send_file_header_to_quit FAIL!");
            goto FAIL;
        }
        ESP_LOGI(TAG, "Send image success");
        reply_server(0x0d, OTA_SUCCESS, stm32Versions);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Send RUN_APP order to stm32 now.");
        uint8_t order = CHECK_RESULT;
        uint8_t reply = send_order_to_stm32(order);
        if (reply == IAP_SUCCESS){
            success_times++;
            time_t now = 0;
            time(&now);
            time_end = now;
            sum_time += time_end - time_start;
            average_time = sum_time/success_times;
            ESP_LOGI(TAG, "STM32 run APP successfully!");
            order = RUN_APP;
            send_order_to_stm32(order);  // 直接发命令让它直接运行APP
            reply_server(0x06, OTA_SUCCESS, stm32Versions);
        } else {
            ESP_LOGE(TAG, "STM32 run APP failed!");
            reply_server(0x0d, OTA_FAILED, stm32Versions);
        }
    } 
FAIL:
    esp_http_client_cleanup(http_client);
    if (content_length != binary_file_len) {
        reply_server(0x0d, OTA_FAILED, stm32Versions);
    }
err:
    all_config.iap_start = 0;
    all_config.stm32_state = STM_DOWNLOADED;
    ESP_LOGI(TAG, "Quit upgrade.");
    vTaskDelete(NULL);
}

esp_err_t xmodem_sender_event_handler(esp_xmodem_event_t *evt)
{
    switch(evt->event_id) {
        case ESP_XMODEM_EVENT_ERROR:
            ESP_LOGI(TAG, "ESP_XMODEM_EVENT_ERROR, err_code is 0x%x, heap size %d", esp_xmodem_get_error_code(evt->handle), esp_get_free_heap_size());
            if (esp_xmodem_stop(evt->handle) == ESP_OK) {
                esp_xmodem_start(evt->handle);
            } else {
                ESP_LOGE(TAG, "esp_xmodem_stop fail");
                esp_xmodem_transport_close(evt->handle);
                esp_xmodem_clean(evt->handle);
            }
            break;
        case ESP_XMODEM_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ESP_XMODEM_EVENT_CONNECTED, heap size %d", esp_get_free_heap_size());
            if (xTaskCreate(&http_client_task, "http_client_task", 8192, evt->handle, 5, NULL) == ESP_FAIL) {
                ESP_LOGE(TAG, "http_client_task create fail");
                return ESP_FAIL;
            }
            break;
        case ESP_XMODEM_EVENT_FINISHED:
            ESP_LOGI(TAG, "ESP_XMODEM_EVENT_FINISHED");
            break;
        case ESP_XMODEM_EVENT_ON_SEND_DATA:
            ESP_LOGD(TAG, "ESP_XMODEM_EVENT_ON_SEND_DATA, %d", esp_get_free_heap_size());
            break;
        case ESP_XMODEM_EVENT_ON_RECEIVE_DATA:
            ESP_LOGD(TAG, "ESP_XMODEM_EVENT_ON_RECEIVE_DATA, %d", esp_get_free_heap_size());
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_xmodem_handle_t stm32_serial_xmodem_iap_init(void)
{
    esp_xmodem_transport_config_t transport_config = {
        .baund_rate = 115200,
        #ifdef CONFIG_IDF_TARGET_ESP32
        .uart_num = UART_NUM_1,
        .swap_pin = true,
        .tx_pin = 19,
        .rx_pin = 18,
        // .cts_pin = 15,
        // .rts_pin = 14,
        #endif
    };
    esp_xmodem_transport_handle_t transport_handle = esp_xmodem_transport_init(&transport_config);
    if (!transport_handle) {
        ESP_LOGE(TAG, "esp_xmodem_transport_init fail");
    }

    esp_xmodem_config_t config = {
        .role = ESP_XMODEM_SENDER,
        .event_handler = xmodem_sender_event_handler,
        .support_xmodem_1k = true,
        .user_data_size = 0,
    };
    esp_xmodem_handle_t sender = esp_xmodem_init(&config, transport_handle);

    if (sender) {
        ESP_LOGI(TAG, "esp_xmodem_init pass");
    } else {
        ESP_LOGE(TAG, "esp_xmodem_init fail");
    }

    return sender;
}

void start_stm32_iap(void)
{
    uint8_t order = REBOOT;
    uint8_t reply = send_order_to_stm32(order);
    
    if (reply == MODE_B){
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        order = UPGRADE;
        reply = send_order_to_stm32(order);
        if (reply == MODE_D){
            if (myXmodemHandle) {
                ESP_LOGI(TAG, " Begin stm32 iap task.");
                if (xTaskCreate(&http_client_task2, "http_client_task2", 4096*2, myXmodemHandle, 20, NULL) == ESP_FAIL) {
                    ESP_LOGE(TAG, "http_client_task2 create fail");
                    all_config.stm32_state = STM_DOWNLOADED;
                    all_config.iap_start = 0;
                    reply_server(0x0d, OTA_FAILED, stm32Versions);
                } else {
                    all_config.stm32_state = STM_DOWNLOADING;
                }
            } else {
                ESP_LOGE(TAG, "myXmodemHandle is null!");
                all_config.stm32_state = STM_DOWNLOADED;
                all_config.iap_start = 0;
                reply_server(0x0d, OTA_FAILED, stm32Versions);
            }
        } else {
            ESP_LOGE(TAG, "Send upgrade failed!");
            all_config.stm32_state = STM_DOWNLOADED;
            all_config.iap_start = 0;
            reply_server(0x0d, OTA_FAILED, stm32Versions);
        }
    } else {
        ESP_LOGE(TAG, "Reboot failed!");
        all_config.stm32_state = STM_DOWNLOADED;
        all_config.iap_start = 0;
        reply_server(0x0d, OTA_FAILED, stm32Versions);
    }
}


