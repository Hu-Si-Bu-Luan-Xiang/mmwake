/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "string.h"
#include "all_config.h"
#include "lwip/sockets.h"

#define HASH_LEN 32
#define OTA_URL_SIZE 256
// #define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL "http://192.168.1.31:8072/build/mmwake.bin"
// #define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL  "http://esp32-upgrade-package.oss-cn-guangzhou.aliyuncs.com/mmwake.bin"
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL "http://8.134.111.197:8072/mmwake.bin"

static const char *TAG = "simple_ota_example";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
extern SemaphoreHandle_t MuxSem_TCP_Handle;
extern uint8_t ota_reply;  // 回应服务器标志位
extern uint8_t id_get[2];  // 存放服务器分配的设备id号
extern const uint8_t Versions[3];  // 程序版本号
extern int sock;
extern All_Flags all_config;

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void)
{
    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    // get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");
}

void reply_server(uint8_t sort, uint8_t code, const uint8_t *versions)
{
    uint8_t reply_bag[BAG_LEN] = {0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0a};
    reply_bag[1] = sort;
    reply_bag[2] = id_get[0];
    reply_bag[3] = id_get[1];
    reply_bag[4] = code;
    reply_bag[5] = versions[0];
    reply_bag[6] = versions[1];
    reply_bag[7] = versions[2];


    time_t nowtime = 1;
    time(&nowtime);
    int now_time = (int)nowtime;
    reply_bag[8] =  (uint8_t)(now_time>>24);
    reply_bag[9] =  (uint8_t)(now_time>>16);
    reply_bag[10] = (uint8_t)(now_time>>8);
    reply_bag[11] = (uint8_t)now_time;
    
    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);  // 堵塞等待TCP空闲
    ESP_LOGW(TAG, "获取TCP 6 —— 回复服务器");
    int err = send(sock, reply_bag, BAG_LEN, 0);
    if (err == BAG_LEN) {
        ESP_LOGW(TAG, "回复成功.");
    } else {
        ESP_LOGE(TAG, "回复失败: errno %d", errno);
    }
    xSemaphoreGive(MuxSem_TCP_Handle);
    ESP_LOGW(TAG, "释放TCP");

}

esp_err_t esp_https_ota_show_process(const esp_http_client_config_t *config)
{
    int image_size, download_size;
    if (!config) {
        ESP_LOGE(TAG, "esp_http_client config not found");
        return ESP_ERR_INVALID_ARG;
    }

    esp_https_ota_config_t ota_config = {
        .http_config = config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (https_ota_handle == NULL) {
        return ESP_FAIL;
    }
    esp_app_desc_t new_app_info;
    ESP_ERROR_CHECK(esp_https_ota_get_img_desc(https_ota_handle, &new_app_info)); // 获取新固件信息
    image_size = esp_https_ota_get_image_size(https_ota_handle); // 获取固件大小
    uint8_t last_process = 0;
    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        download_size = esp_https_ota_get_image_len_read(https_ota_handle); // 已经下载 
        uint8_t process = (uint8_t)(download_size * 10 / image_size );
        if (last_process != process && process < 10) {
            reply_server(0x0c, (uint8_t)(process+10), Versions);
            ESP_LOGW(TAG, "Image download process: %d / %d --- %0.1f%%", download_size, image_size, (float)download_size * 100 / image_size); // 下载进度(百分比)
            last_process = process;
        }
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(https_ota_handle);
        return err;
    }

    esp_err_t ota_finish_err = esp_https_ota_finish(https_ota_handle);
    if (ota_finish_err != ESP_OK) {
        return ota_finish_err;
    }

    return ESP_OK;
}

void simple_ota_example_task(void *pvParameter)
{
    while(1) 
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // OTA请求发生时，OTA任务的优先级最高，需要占用TCP资源（用all_config.ota_start配合）
        xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);  
        ESP_LOGW(TAG, "获取TCP - 开始OTA模式");
        xSemaphoreGive(MuxSem_TCP_Handle);
        ESP_LOGW(TAG, "释放TCP");

        if (ota_reply == OTA_START){
            reply_server(0x0c, OTA_START, Versions);
            ESP_LOGW(TAG, "开始升级");

            get_sha256_of_partitions();

            esp_http_client_config_t config = {
                .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
                .cert_pem = (char *)server_cert_pem_start,
                .event_handler = _http_event_handler,
                .keep_alive_enable = true,
            };

            #ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN  // 用于配置固件升级时是否从标准输入（stdin）中读取固件升级文件 URL 的选项,这里固定URL，不用手动输入
                char url_buf[OTA_URL_SIZE];
                if (strcmp(config.url, "FROM_STDIN") == 0) {
                    example_configure_stdin_stdout();
                    fgets(url_buf, OTA_URL_SIZE, stdin);
                    int len = strlen(url_buf);
                    url_buf[len - 1] = '\0';
                    config.url = url_buf;
                } else {
                    ESP_LOGE(TAG, "Configuration mismatch: wrong firmware upgrade image url");
                    abort();
                }
            #endif

            #ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK  // 用于配置 HTTPS 连接时是否需要跳过服务器证书中 Common Name 字段检查的选项
                config.skip_cert_common_name_check = true;
            #endif

            // esp_err_t ret = esp_https_ota(&config);
            esp_err_t ret = esp_https_ota_show_process(&config);

            if (ret == ESP_OK) {
                reply_server(0x0c, OTA_SUCCESS, Versions);
                vTaskDelay(1000 / portTICK_RATE_MS);
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Firmware upgrade failed");
                reply_server(0x0c, OTA_FAILED, Versions);
                all_config.ota_start = false;
                all_config.someone_changed = true;
            }
        }

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

// void app_main(void)
// {
//     // Initialize NVS.
//     esp_err_t err = nvs_flash_init();
//     if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
//         // partition table. This size mismatch may cause NVS initialization to fail.
//         // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
//         // If this happens, we erase NVS partition and initialize NVS again.
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         err = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(err);

//     get_sha256_of_partitions();

//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());

//     /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
//      * Read "Establishing Wi-Fi or Ethernet Connection" section in
//      * examples/protocols/README.md for more information about this function.
//      */
//     ESP_ERROR_CHECK(example_connect());

// #if CONFIG_EXAMPLE_CONNECT_WIFI
//     /* Ensure to disable any WiFi power save mode, this allows best throughput
//      * and hence timings for overall OTA operation.
//      */
//     esp_wifi_set_ps(WIFI_PS_NONE);
// #endif // CONFIG_EXAMPLE_CONNECT_WIFI

//     xTaskCreate(&simple_ota_example_task, "ota_example_task", 8192, NULL, 5, NULL);
// }
