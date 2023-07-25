#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "all_config.h"
#include "serial.h"
#include "simple_ota_example.h"
#include "xmodem_sender_example.h"
#include "key.h"
#include "led.h"
#include "save.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "smart_config.h"
#include "tcp.h"
#include "time_world.h"
#include "esp_xmodem.h"

QueueHandle_t uart_event_queue;
esp_xmodem_handle_t myXmodemHandle;

// 用于初始化一个解开的锁
portMUX_TYPE mmux = portMUX_INITIALIZER_UNLOCKED;

/// @brief TXT文件互斥量，SMART互斥量
SemaphoreHandle_t MuxSem_TXT_Handle = NULL;
SemaphoreHandle_t MuxSem_SMART_Handle = NULL;
SemaphoreHandle_t MuxSem_TCP_Handle = NULL;
SemaphoreHandle_t MuxSem_UART_Handle = NULL;

TaskHandle_t Smart_task_handle = NULL;
TaskHandle_t TCP_task_handle = NULL;
TaskHandle_t KEEP_task_handle = NULL;
TaskHandle_t update_time_task_handle = NULL;
TaskHandle_t stay_for_check_wifi_for_server_handle = NULL;
TaskHandle_t stay_for_stress_test_for_server_handle = NULL;
TaskHandle_t ReadFile_TCP_handle = NULL;
TaskHandle_t OTA_handle = NULL;
TaskHandle_t IAP_handle = NULL;
EventGroupHandle_t s_wifi_event_group;

// 程序版本号Versions X.Y.Z [主版本号.次版本号.修订号]， 具体版本迭代看readme.md文件
const uint8_t Versions[3] = {1, 2, 3};  
uint8_t stm32Versions[3] = {1, 0, 0}; 
/// @brief 初始化标志位
All_Flags all_config = {
    .wifi_connect = false,
    .ip_get = false,
    .tcp_connect = false,
    .time_get = false,
    .smart_start = false,
    .ota_start = false,
    .iap_start = false,
    .id_get = false,
    .file_fine = 1,
    .reboot_fisrt_time = true,
    .reconnect_fisrt_time = false,
    .receive_last_timestamp = false,
    .someone_changed = false,
    .stm_radar_failed = false,
    .rev_bag = 0,
    .stm32_state = STM_DISCONNECT  // 默认上电状态时是失联状态
};

/// @brief 所有的部件初始化
/// @param  
void init(void) {
    //————————————————————————创建TXT的资源锁、TIMER
    
    MuxSem_UART_Handle = xSemaphoreCreateMutex();
    #ifdef DEBUG
    if (NULL != MuxSem_UART_Handle) 
        printf("MuxSem_UART_Handle 互斥量创建成功!\r\n"); 
    #endif

    MuxSem_TXT_Handle = xSemaphoreCreateMutex();
    #ifdef DEBUG
    if (NULL != MuxSem_TXT_Handle) 
        printf("MuxSem_TXT_Handle 互斥量创建成功!\r\n"); 
    #endif

    MuxSem_TCP_Handle = xSemaphoreCreateMutex();
    #ifdef DEBUG
    if (NULL != MuxSem_TCP_Handle) 
        printf("MuxSem_TCP_Handle 互斥量创建成功!\r\n");
    #endif 

    MuxSem_SMART_Handle = xSemaphoreCreateMutex();
    #ifdef DEBUG
    if (NULL != MuxSem_SMART_Handle) 
        printf("MuxSem_SMART_Handle 互斥量创建成功!\r\n"); 
    #endif
    xSemaphoreTake(MuxSem_SMART_Handle, portMAX_DELAY);
    
    // KEEP_task_handle = xSemaphoreCreateMutex();
    // #ifdef DEBUG
    // if (NULL != KEEP_task_handle) 
    //     printf("KEEP_task_handle 互斥量创建成功!\r\n"); 
    // xSemaphoreTake(KEEP_task_handle, portMAX_DELAY);
    // #endif


    //————————————————————————nvs初始化分区
    ESP_ERROR_CHECK(nvs_flash_init());
    //————————————————————————初始化wifi
    initialise_wifi();
    //————————————————————————串口初始化
    // serial_init();
    myXmodemHandle = stm32_serial_xmodem_iap_init();
    //————————————————————————按钮初始化
    key_init();

    //————————————————————————LED测试
    //test_led();

    //————————————————————————文件初始化
    fatfs_init();
    //test_fatfs_out();

    ESP_LOGW("TAG", "当前程序版本：Versions %d.%d.%d", Versions[0], Versions[1], Versions[2]);
    
}

void app_main(void)
{
    //初始化
    init();
    xTaskCreate(rx_task, "uart_rx_task", 4096*3, NULL, 23, NULL);
    xTaskCreate(ReadFile_TCP, "ReadFile_TCP", 3072*7, NULL, configMAX_PRIORITIES, &ReadFile_TCP_handle);
    xTaskCreate(smartconfig_example_task, "smartconfig_task", 4096, NULL, 23, &Smart_task_handle);
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 23, &TCP_task_handle);
    xTaskCreate(LED_task, "LED_task", 1024, NULL, 20, NULL);
    xTaskCreate(keep_connect, "keep_connect", 3072*2, NULL, 20, NULL);
    xTaskCreate(update_time_task, "update_time", 2048*2, NULL, 21, &update_time_task_handle);
    xTaskCreate(stay_for_check_wifi_for_server, "get_wifi_rssi", 2048*2, NULL, 22, & stay_for_check_wifi_for_server_handle);
    xTaskCreate(stay_for_stress_test_for_server, "stress_test", 2048*2, NULL, 22, &stay_for_stress_test_for_server_handle);
    xTaskCreate(&simple_ota_example_task, "ota_example_task", 8192, NULL, 24, &OTA_handle);

}
