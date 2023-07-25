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
#include "esp_sntp.h"
#include "all_config.h"
#include "lwip/sockets.h"
#include "save/include/save.h"

static const char *TAG = "GET_TIME";
static void obtain_time(void);
static void initialize_sntp(void);

extern All_Flags all_config;
extern TaskHandle_t TCP_task_handle;
extern SemaphoreHandle_t MuxSem_TXT_Handle;
extern SemaphoreHandle_t MuxSem_TCP_Handle;
extern int boot_time;
extern int sock;

#ifdef DEBUG
extern char str_boot_time[64];
extern uint8_t id_get[2];
extern const uint8_t Versions[3];   // 程序版本号
u32_t rectified_times = 0;          // 校正次数
u32_t rectified_nums_sum = 0;       // 累积校正秒数
int16_t rectified_nums_min = 0;     // 单次校正中最小校正的秒数
int16_t rectified_nums_max = 0;     // 单次校正中最大校正的秒数
uint8_t max_retry_times = 0;        // 最大同步校正次数
#endif

void time_init(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year < (2022 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
    ESP_LOGI(TAG, "World time was gotten!");
        
    // Set timezone to China Standard Time
    setenv("TZ", "CST-8", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    #ifdef DEBUG
    strftime(str_boot_time, sizeof(str_boot_time), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in Shanghai is: %s", str_boot_time);
    #endif
    boot_time = now;
    send_reboot_bag();
    all_config.time_get = true;
    all_config.someone_changed = true;
}

static void obtain_time(void)
{
    initialize_sntp();
    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... ( %d / %d )", retry, retry_count);
        if(retry == 9){
            //堵塞获取各任务句柄限权，等待tcp、txt等任务完成当前操作再重启，避免TCP发送失败、文件数据出错
            // xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
            // xSemaphoreTake(MuxSem_TXT_Handle,portMAX_DELAY);
            // xSemaphoreGive(MuxSem_TXT_Handle);
            ESP_LOGE(TAG, "Obtain time failed, rebooting esp32.");
            ESP_LOGE("Obtain_Time","—————————————————————————————————————授——时——失——败—————————————————————————————————————————————");
            esp_restart();
        }
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);
}

void update_time(void)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    uint8_t retry_count = 0 , max_retry_count = 10;

    time(&now);
    localtime_r(&now, &timeinfo);
    #ifdef DEBUG
    char strftime_buf[64];
    int time_before_update, time_after_update, sub_time;
    time_before_update = now;
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGW("Updated time", "Before updated time: %s", strftime_buf);
    #endif

    if(timeinfo.tm_year >= (2022 - 1900)) { //只更新2022年之后的时间
        initialize_sntp(); // 重新同步时间
        while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry_count++ < max_retry_count) 
        {
            time(&now);
            localtime_r(&now, &timeinfo);
            #ifdef DEBUG
            time_before_update = now;
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGW("Updated time", "Before updated time: %s", strftime_buf);
            ESP_LOGW("Updated time", "Waiting for system time to be updated... ( %d / %d )", retry_count, max_retry_count);
            #endif
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
    #ifdef DEBUG
    time(&now);
    localtime_r(&now, &timeinfo);
    time_after_update = now;
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGW("Updated time", "After updated time: %s", strftime_buf);
    sub_time = time_after_update - time_before_update - 2;
    if (abs(sub_time) > 1){
        rectified_times += 1;
        rectified_nums_sum += abs(sub_time);
        if(sub_time < rectified_nums_min){
            rectified_nums_min = sub_time;
        }
        if(sub_time > rectified_nums_max){
            rectified_nums_max = sub_time;
        }
        
    }
    if(retry_count > max_retry_times) {
        max_retry_times = retry_count;
    }
    #endif
}

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGW(TAG, "Notification of a time synchronization event.");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    if (sntp_enabled())
    {
        sntp_restart();
    } else {
        sntp_setservername(0, "pool.ntp.org");
        // SNTP_OPMODE_POLL表示单播模式，SNTP_OPMODE_LISTENONLY表示广播模式
        sntp_setoperatingmode(SNTP_OPMODE_POLL); 
        sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
        // SNTP_SYNC_MODE_IMMED: 每次调用 sntp_get_current_timestamp 时都会立即执行一次同步校正；
        // SNTP_SYNC_MODE_SMOOTH: 基于当前时钟时间和上次同步校正的时间之间的误差大小以及时间长度计算出一个误差加权平均值，只有当误差大于这个平均值才会触发同步校正。
        sntp_init();
    }
}

void update_time_task(void *pvParameters)
{
    u16_t i = 0;
    while(1)
    {
        if(all_config.ip_get){
            if(!all_config.time_get){
                // 只有当连接上WIFI且没有获取到世界时间时才执行获取时间这个操作
                ESP_LOGI("Obtain_Time","———————————————————————————————————获——取——时——间————————————————————————————————————————————————");
                time_init();
                ESP_LOGI("Obtain_Time","———————————————————————————————————授——时——成——功————————————————————————————————————————————————");
            } else {
                // 定时校正
                if(i % 1200 == 0){
                    i = 0;
                    ESP_LOGW("Update_Time","———————————————————————————————————校——正——时——间————————————————————————————————————————————————");
                    update_time();
                    #ifdef DEBUG
                    ESP_LOGW("Update_Time","———————————————————————————————————校——正——时——间————————————————————————————————————————————————");
                    ESP_LOGW("Retified_Time", " retry: %d ; times: %d ; sum: %d ; min: %d ; max: %d .", max_retry_times, rectified_times, rectified_nums_sum, rectified_nums_min, rectified_nums_max);
                    #endif
                }
            }
        }
        #ifdef DEBUG
        if(all_config.someone_changed) {
            printf(" Versions: %d.%d.%d ; ID: %d ; ota_start: %d ; smart_start: %d ; ip_get: %d ; tcp_connect: %d ; time_get: %d ; file_fine: %d ; reboot: %d ; reconnect: %d ; receive_last: %d .\n",
                    Versions[0], Versions[1], Versions[2], id_get[0]*256 + id_get[1], all_config.ota_start, all_config.smart_start, all_config.ip_get, all_config.tcp_connect, all_config.time_get, all_config.file_fine, all_config.reboot_fisrt_time, all_config.reconnect_fisrt_time, all_config.receive_last_timestamp);
            all_config.someone_changed = false; // 打印完后清零
        }
        #endif
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        i++;
    }
    
}
