#include <stdio.h>
#include "led.h"
#include "esp_system.h"
#include "esp_log.h"
#include "led_indicator.h"
#include "unity.h"
#include "all_config.h"
#include "driver/gpio.h"

extern All_Flags all_config;
#ifdef LED_NEW_BOARD
#else
#define LED_IO_NUM_0    LED_IO_2
#define LED_IO_NUM_1    LED_IO_1
#endif

#define LED_TURN_OFF    LED_TURN_OFF_MY
#define LED_TURN_ON    LED_TURN_ON_MY

#define TAG "led indicator"


void led_indicator_init()
{
    #ifdef LED_NEW_BOARD
    gpio_pad_select_gpio(LED_IO_ORANGE);
    gpio_set_direction(LED_IO_ORANGE, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(LED_IO_BLUE);
    gpio_set_direction(LED_IO_BLUE, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(LED_IO_RED);
    gpio_set_direction(LED_IO_RED, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(LED_IO_GREEN);
    gpio_set_direction(LED_IO_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_IO_ORANGE, LED_TURN_OFF);
    gpio_set_level(LED_IO_BLUE, LED_TURN_OFF);
    gpio_set_level(LED_IO_RED, LED_TURN_OFF);
    gpio_set_level(LED_IO_GREEN, LED_TURN_OFF);
    #else
    gpio_set_direction(LED_IO_NUM_0, GPIO_MODE_OUTPUT);//写这个或下一个
    gpio_set_direction(LED_IO_NUM_1, GPIO_MODE_OUTPUT);//或这个
    //1为高电平，0为低电平
    gpio_set_level(LED_IO_NUM_0, LED_TURN_OFF);
    gpio_set_level(LED_IO_NUM_1, LED_TURN_OFF);
    #endif
}

void test_led(void)
{
    led_indicator_init();
}

/// @brief 绿灯闪烁——smartconfig  LED1常灭——正常   LED1闪烁——开始smartconfig  LED1常亮——网络不顺   LED2闪烁，tcp服务器跪了
/// @param pvParameters 
void LED_task(void *pvParameters)
{
    led_indicator_init();
    bool state_led_orange = 1, state_led_blue = 0, state_led_red = 0, state_led_green = 0;
    while (1)
    {
        #ifdef LED_NEW_BOARD
        // 橙灯 ：      --- 闪烁：未连接上Wifi              --- 常亮：连接不上服务器             --- 常灭：连接上wifi且连接上服务器

        // 蓝灯 ：      --- 闪烁：进入配网模式              --- 常亮：未获取到时间               --- 常灭：获取到了世界时间

        // 红灯 ：      --- 闪烁：文件系统损坏              --- 常亮：雷达通讯失败               --- 常灭：工作正常

        // 绿灯 ：      --- 闪烁：正常工作                  --- 常亮：正在进行固件升级           --- 常灭：STM32工作异常
        
        if(all_config.ip_get){
            if(all_config.tcp_connect){
                gpio_set_level(LED_IO_ORANGE, LED_TURN_OFF);
                state_led_orange = LED_TURN_OFF;
            }else{
                gpio_set_level(LED_IO_ORANGE, LED_TURN_ON);
                state_led_orange = LED_TURN_ON;
            }
        }else{
            state_led_orange = !state_led_orange;
            gpio_set_level(LED_IO_ORANGE, state_led_orange);
        }

        if(all_config.smart_start){
            state_led_blue = !state_led_blue;
            gpio_set_level(LED_IO_BLUE, state_led_blue);
        }else{
            if(all_config.time_get){
                gpio_set_level(LED_IO_BLUE, LED_TURN_OFF);
                state_led_blue = LED_TURN_OFF;
            }else{
                gpio_set_level(LED_IO_BLUE, LED_TURN_ON);
                state_led_blue = LED_TURN_ON;
            }
        }
        
        if(all_config.file_fine == 0){
            state_led_red = !state_led_red;
            gpio_set_level(LED_IO_RED, state_led_red);
        }else{
            if(all_config.stm_radar_failed){
                gpio_set_level(LED_IO_RED, LED_TURN_ON);
                state_led_red = LED_TURN_ON;
                all_config.stm_radar_failed = false;
            }else{
                gpio_set_level(LED_IO_RED, LED_TURN_OFF);
                state_led_red = LED_TURN_OFF;
            }
        }

        if(all_config.ota_start){
            gpio_set_level(LED_IO_GREEN, LED_TURN_ON);
            state_led_green = LED_TURN_ON;
        }else{
            if(all_config.rev_bag > 0){
                state_led_green = !state_led_green;
                gpio_set_level(LED_IO_GREEN, state_led_green);
                all_config.rev_bag = 0;
            }else{
                gpio_set_level(LED_IO_GREEN, LED_TURN_OFF);
                state_led_green = LED_TURN_OFF;
            }
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);

        #else
        //   LED_IO_NUM_0   红灯   网络故障指示灯
        //   LED_IO_NUM_1   蓝灯   配网指示灯
        //   配网模式下
        if(all_config.smart_start)
        { 
            if(!all_config.ip_get){
                // 配网开始至连上wifi前，配网灯、网络故障灯交替闪烁
                gpio_set_level(LED_IO_NUM_0, LED_TURN_OFF);
                gpio_set_level(LED_IO_NUM_1, LED_TURN_ON);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                gpio_set_level(LED_IO_NUM_1, LED_TURN_OFF); 
                gpio_set_level(LED_IO_NUM_0, LED_TURN_ON);
            }else{
                if(!all_config.time_get){
                    // 连上wifi,但没有获取到时间，配网灯常亮
                    gpio_set_level(LED_IO_NUM_1, LED_TURN_ON);
                }else{
                    // 连上wifi且获取到时间，配网灯灭
                    gpio_set_level(LED_IO_NUM_1, LED_TURN_OFF);                  
                }
                if(!all_config.tcp_connect){
                    // 连上wifi且获取到时间但TCP连不上，网络故障灯常亮
                    gpio_set_level(LED_IO_NUM_0, LED_TURN_ON);
                }else{
                    // 连上wifi且获取到时间且TCP连上，网络故障灯灭
                    gpio_set_level(LED_IO_NUM_0, LED_TURN_OFF);
                }
            } 
        }else{
        //   正常wifi模式下
            if(!all_config.time_get){
                // 连上wifi,但没有获取到时间，配网灯常亮
                gpio_set_level(LED_IO_NUM_1, LED_TURN_ON);
            }else{
                // 连上wifi且获取到时间，配网灯灭
                gpio_set_level(LED_IO_NUM_1, LED_TURN_OFF);                  
            }
            if(!all_config.ip_get){
                // wifi断开连接，网络故障灯闪烁
                gpio_set_level(LED_IO_NUM_0, LED_TURN_ON);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                gpio_set_level(LED_IO_NUM_0, LED_TURN_OFF);
            }else{
                if(!all_config.tcp_connect){
                    // 连上wifi且获取到时间但TCP连不上，网络故障灯常亮
                    gpio_set_level(LED_IO_NUM_0, LED_TURN_ON);
                }else{
                    // 连上wifi且获取到时间且TCP连上，网络故障灯灭
                    if(all_config.file_fine){
                        gpio_set_level(LED_IO_NUM_0, LED_TURN_OFF);
                    }else{
                        gpio_set_level(LED_IO_NUM_0, LED_TURN_ON);
                        gpio_set_level(LED_IO_NUM_1, LED_TURN_ON);
                        vTaskDelay(500 / portTICK_PERIOD_MS);
                        gpio_set_level(LED_IO_NUM_1, LED_TURN_OFF); 
                        gpio_set_level(LED_IO_NUM_0, LED_TURN_OFF);
                        // 当网络是好的情况下，文件系统损坏时，网络故障灯和配网灯一起闪烁！
                    }
                }
            } 
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
        #endif
    }   
}

