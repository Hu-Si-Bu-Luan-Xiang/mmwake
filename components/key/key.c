#include <stdio.h>
#include "key.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "unity.h"
#include "iot_button.h"
#include "sdkconfig.h"
#include "../save/include/save.h"
#include "../serial/include/serial.h"
#include "lwip/sockets.h"
#include "all_config.h"

static const char *TAG = "BUTTON TEST";
extern TaskHandle_t Smart_task_handle;
extern All_Flags all_config;
extern EventGroupHandle_t s_wifi_event_group;
extern SemaphoreHandle_t MuxSem_TXT_Handle;
extern SemaphoreHandle_t MuxSem_TCP_Handle;
static const int PRESS_KEY_BIT = BIT2;

static uint8_t once = 1;
extern int sock;

#define BUTTON_IO_NUM  BUTTON_IO_NUM_MY
#define BUTTON_ACTIVE_LEVEL   BUTTON_ACTIVE_LEVEL_MY
#define BUTTON_NUM 1

#define CONFIG_BUTTON_SHORT_PRESS_TIME_MS_MY 180
#define CONFIG_BUTTON_LONG_PRESS_TIME_MS_MY 700

#define LONG_PRESS_TONET_TIME_S LONG_PRES_TIME_S

static button_handle_t g_btns[BUTTON_NUM] = {0};

static int get_btn_index(button_handle_t btn)
{
    for (size_t i = 0; i < BUTTON_NUM; i++) {
        if (btn == g_btns[i]) {
            return i;
        }
    }
    return -1;
}

static void button_single_click_cb(void *arg, void *data)
{
    TEST_ASSERT_EQUAL_HEX(BUTTON_SINGLE_CLICK, iot_button_get_event(arg));
    ESP_LOGI(TAG, "BTN%d: BUTTON_SINGLE_CLICK", get_btn_index((button_handle_t)arg));
}
static void button_up_cb(void *arg, void *data)
{
    TEST_ASSERT_EQUAL_HEX(BUTTON_PRESS_UP, iot_button_get_event(arg));
    ESP_LOGW(TAG, "BTN_up");
    once = 1;
}
static void button_long_press_hold_cb(void *arg, void *data)
{
    TEST_ASSERT_EQUAL_HEX(BUTTON_LONG_PRESS_HOLD, iot_button_get_event(arg));
    //ESP_LOGI(TAG, "BTN%d: BUTTON_LONG_PRESS_HOLD[%d],count is [%d]", get_btn_index((button_handle_t)arg), iot_button_get_ticks_time((button_handle_t)arg), iot_button_get_long_press_hold_cnt((button_handle_t)arg));

    if(iot_button_get_long_press_hold_cnt((button_handle_t)arg) > LONG_PRESS_TONET_TIME_S * 70 && once)
    {
        ESP_LOGW(TAG, "---------------------------SmartConfig Key was pressed.---------------------------");
        once = 0;
        
        if(all_config.ip_get && all_config.tcp_connect && sock != -1) // 如果有网络直接发
        {
            uint8_t data_cood[BAG_LEN] = {0x5a, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0a};
            xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);//堵塞等待可写
            ESP_LOGW(TAG, "获取TCP 发送配网按钮事件");
            int err = send(sock, data_cood, BAG_LEN, 0);
            xSemaphoreGive(MuxSem_TCP_Handle);
            ESP_LOGW(TAG, "释放TCP ");
            if(err == BAG_LEN){
                ESP_LOGW(TAG, "发送成功。");
            }else{
                ESP_LOGE(TAG, "发送失败。");
            }
        }
        ESP_LOGW(TAG, "---------------------------SmartConfig Key was released.---------------------------");   
        // 设置事件组配网按钮按下标志位
        xEventGroupSetBits(s_wifi_event_group, PRESS_KEY_BIT);
    }
}

void key_init(void)
{
    button_config_t cfg = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS_MY,
        .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS_MY,
        .gpio_button_config = {
            .gpio_num = BUTTON_IO_NUM,
            .active_level = 0,
        },
    };
    g_btns[0] = iot_button_create(&cfg);
    TEST_ASSERT_NOT_NULL(g_btns[0]);
    iot_button_register_cb(g_btns[0], BUTTON_SINGLE_CLICK, button_single_click_cb, NULL);
    iot_button_register_cb(g_btns[0], BUTTON_PRESS_UP,button_up_cb,NULL);
    iot_button_register_cb(g_btns[0], BUTTON_LONG_PRESS_HOLD, button_long_press_hold_cb, NULL);
}