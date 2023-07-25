#include <stdio.h>
#include "serial.h"
#include "esp_log.h"
#include "all_config.h"
#include "save.h"
#include "xmodem_sender_example.h"
#include "esp_xmodem_transport.h"
#include "ota/include/simple_ota_example.h"

#define TXD_PIN TX_PIN
#define RXD_PIN RX_PIN
#define RECEIVE_BUF_LEN  Serial_RX_BUF_LEN   
//设置buffer_size用于避免过于频繁的调32位的信息到使用
static const int RX_BUF_SIZE = Serial_RX_BUF_LEN;
static const int TX_BUF_SIZE = Serial_TX_BUF_LEN;

// 创建一个接收串口数据的空间
uint8_t data_recive_bytes[RECEIVE_BUF_LEN];

//全局的flag
extern All_Flags all_config;
extern uint8_t stm32Versions[3];          // STM32程序版本号
extern int boot_time;
extern SemaphoreHandle_t MuxSem_UART_Handle;
static const char *RX_TASK_TAG = "RX_TASK";
const char *file_path =  TXT_PATH;
uint8_t id_get[2];                           // 存放服务器分配的设备id号
uint8_t id_coodINBAG[2];                     // 此处需要耦合到获取ID的地方
int last_time = 0;                           // 记录上一次接收到正常数据包的时间戳
int last_repeat_time = 0;                    // 记录上一次出现重复的时间戳
int last_losses_time = 0;                    // 记录上一次出现丢失的时间戳
u16_t num_busy = 0;                          // 待写入文件缓冲区的包数
uint8_t num_bag_to_cood = 0;                 // 正常数据缓冲区 data_out_cood 的待处理包数
int success_times = 0, time_start, time_end, average_time, sum_time = 0;

#ifdef DEBUG
u32_t num_repeat_time = 0;                     // 累计时间戳重复的数据包个数
u32_t num_losses_time = 0;                     // 累计时间戳丢失的数据包个数
u32_t num_rectified_repeat = 0;                // 修正时间戳重复的数据包个数
u32_t num_rectified_losses = 0;                // 修正时间戳丢失的数据包个数
u32_t stm_reset_times = 0;                     // 收到STM32重启数据包的个数
u32_t stm_radar_failed_times = 0;              // 收到雷达通讯异常数据包个数
u32_t stm_data_bags = 0;                       // 收到STM32正常数据包的个数
extern long bytes_receive_from_stm;            // 收到STM32数据包的总字节数
#endif


//stm32-esp32   
// 通讯协议：正常数据  0x5a 0xa5 xx xx xx xx 0x0d 0x0a 
// 通讯协议：STM重启   0x5a 0x01 xx xx xx xx 0x0d 0x0a 
// 通讯协议：radar异常 0x5a 0x10 xx xx xx xx 0x0d 0x0a 
// esp32的空闲中断
// 启用了空闲中断功能：在 uart_driver_install() 函数中通过设置 uart_driver_install_config_t 结构体中的 use_event 参数为 true 来启用空闲中断。
// 有数据接收：串口接收到的数据已经达到了 uart_driver_install_config_t 结构体中的 rx_buffer_size 参数定义的缓冲区大小，或者在接收到完整的一帧数据之后。

// 串口的事件句柄
QueueHandle_t *uart_event_queue_handle;
extern QueueHandle_t uart_event_queue;

// 串口初始化函数
void serial_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        // 指定了校验位的类型。在这里，UART_PARITY_DISABLE表示不使用校验位
        // 其他可能的值包括UART_PARITY_EVEN和UART_PARITY_ODD，分别表示偶校验和奇校验。校验位用于检测串口数据传输中的错误，可以提高数据传输的可靠性
        .parity = UART_PARITY_DISABLE,
        // 使用 1 位停止位，即每个数据包发送完毕后发送一个停止位。这是 UART 协议中的一个基本配置项。在 UART 协议中
        // 停止位的作用是告诉接收端一个字符的传输已经结束，准备开始接收下一个字符。
        .stop_bits = UART_STOP_BITS_1,

        // 因为硬件流控需要很牛逼的东西，因此不用
        // .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        // UART_SCLK_APB 是 UART 外设时钟源选择的一种选项，表示使用 APB 时钟作为 UART 时钟源。APB 时钟是一个外设总线时钟，其频率通常是系统时钟频率的一半，可以通过 CONFIG_ESP32_DEFAULT_CPU_FREQ_XXX 宏来配置。当将 source_clk 设置为 UART_SCLK_APB 时，UART 外设的时钟将与 APB 时钟同步，即 UART 时钟频率等于 APB 时钟频率
        // 从而实现了简单的时钟配置。其他可选的时钟源包括 UART_SCLK_REF_TICK 和 UART_SCLK_APB_CLK，它们分别使用参考时钟和定时器时钟作为 UART 时钟源。
        .source_clk = UART_SCLK_APB,
    };

    // uart号，接收缓冲区大小，发送缓冲区大小，队列大小，队列句柄，中断分配标志，
    // UART中断分配标志，如果需要在中断中处理UART数据，可以设置该参数为ESP_INTR_FLAG_IRAM或ESP_INTR_FLAG_LOWMED，表示中断处理程序可以运行在IRAM或低中速内存中。
    // 如果要使用事件，就要使用事件队列,创建队列，每个的成员是uart_event_t

    uart_event_queue = xQueueCreate(10, sizeof(uart_event_t));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, RX_BUF_SIZE, TX_BUF_SIZE, 10, &uart_event_queue, 0));
    // 配置参数赋予
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    // 设置tx rx 流控位脚
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(RX_TASK_TAG, "串口初始化成功-串口为TX: IO19 | RX: IO18");
}


/// @brief 使用串口发送，
/// @param logName 需要发送的发送后的log
/// @param data 需要发送的data——会自动处理成字符串
/// @return 发送成功的大小
int sendData(const char* logName, const char* data)
{
    const int len = strlen(data);
    const int txBytes = uart_write_bytes(UART_NUM_1, data, len);
    ESP_LOGI(logName, "Wrote %d bytes", txBytes);
    return txBytes;
}

/// @brief 是一个发送任务，例程用的
/// @param arg 
// void tx_task(void *arg)
// {
//     static const char *TX_TASK_TAG = "TX_TASK";
//     esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);
//     while (1) {
//         //sendData(TX_TASK_TAG, "Hello world\r\n");
//         (2000 / portTICK_PERIOD_MS);
//     }
// }


/// @brief 将信息打包，为发送做准备——注意使用的时候要上锁，因为可能————每帧数据BAG_LEN字节
/// @param data 收到的信息
/// @param id 需要添加的id
/// @param len 要处理的长度 
/// @param target 目标数组
/// @param save_bit 留用位
///  函数返回值： 0：没打包完成  1：打包了一个包  2：打包了两个包
uint8_t cood_data(const uint8_t * data, uint8_t *target, const uint8_t * save_bit, const uint8_t sort)
{
    uint8_t return_bag_num = 1; // 返回打包好的包数目
    
    // 获取最新更新的的ID
    id_coodINBAG[0] = id_get[0];
    id_coodINBAG[1] = id_get[1];
    // ESP_LOGI(RX_TASK_TAG, "--------------------------设备ID  %x %x --------------------------------",id_coodINBAG[0],id_coodINBAG[1]);

    // 帧头帧尾
    uint8_t start[2]={
        HEAD_SEND1,
    };

    uint8_t end[2]={
        END_SEND1,
        END_SEND2,
    };

    // 时间戳打包
    uint8_t time_buf[4] = {0};
    time_t nowtime = 1;
    time(&nowtime);
    int now_time = (int)nowtime;
    
    if(sort == 0x01){

        if(num_bag_to_cood == 0){  // data_out_cood 缓冲区没有待处理数据
            if(now_time == last_time){
                // 时间戳重复
                #ifdef DEBUG
                num_repeat_time += 1;
                ESP_LOGW("Repeat Time", "--------------- Cooding repeat time : %d   repeat_num: %d   sum: %d ---------------", now_time, 1, num_repeat_time);
                #endif
                last_repeat_time = now_time;
                num_bag_to_cood = 1;
                return_bag_num = 0;
            }else if(now_time - last_time > 1 && last_time != 0){
                // 时间戳丢失
                #ifdef DEBUG
                num_losses_time += now_time - last_time - 1;
                ESP_LOGW("Loss   Time", "--------------- Cooding losses time : %d   losses_num: %d   sum: %d ---------------", now_time-1, now_time-last_time-1, num_losses_time);
                #endif
                last_losses_time = now_time-1;
                num_bag_to_cood = 1;
                return_bag_num = 0;
            }else{  
                // 正常情况
                return_bag_num = 1;
            }
            // 打包
            time_buf[0] = (uint8_t)(nowtime>>24);
            time_buf[1] = (uint8_t)(nowtime>>16);
            time_buf[2] = (uint8_t)(nowtime>>8);
            time_buf[3] = (uint8_t)nowtime;
            memcpy(target + 0,  start   , 1);
            target[1] = sort;
            target[2] = id_coodINBAG[0];
            target[3] = id_coodINBAG[1];
            memcpy(target + 4,  data    , 4);
            memcpy(target + 8,  time_buf, 4);
            memcpy(target + 12, save_bit, 2);
            memcpy(target + 14, end,      2);
        }else if(num_bag_to_cood == 1){
            ESP_LOGW(RX_TASK_TAG, "--------------------------------修正时间戳cood协议帧内存-------------------------------------");
            ESP_LOG_BUFFER_HEXDUMP("Before fixing:", target, BAG_LEN, ESP_LOG_WARN);
            if(now_time == last_time){
                // 时间戳重复
                #ifdef DEBUG
                num_repeat_time += 1;
                ESP_LOGW("Repeat Time", "--------------- Cooding repeat time : %d   repeat_num: %d   sum: %d ---------------", now_time, 1, num_repeat_time);
                #endif
                if(now_time - last_losses_time == 1){  // 重复的刚好是上一次丢失的
                    time_buf[0] = (uint8_t)(last_losses_time>>24);
                    time_buf[1] = (uint8_t)(last_losses_time>>16);
                    time_buf[2] = (uint8_t)(last_losses_time>>8);
                    time_buf[3] = (uint8_t)last_losses_time;
                    #ifdef DEBUG
                    ESP_LOGW(RX_TASK_TAG, "重复的时间戳刚好是上一次丢失的，修正时间戳...");
                    num_rectified_repeat += 1;
                    #endif
                    memcpy(target + 8,  time_buf, 4);  // 修正时间戳     
                }
                last_repeat_time = now_time;
            }else if(now_time - last_time > 1 && last_time != 0){
                // 时间戳丢失
                #ifdef DEBUG
                num_losses_time += now_time - last_time - 1;
                ESP_LOGW("Loss   Time", "--------------- Cooding losses time : %d   losses_num: %d   sum: %d ---------------", now_time-1, now_time-last_time-1, num_losses_time);
                #endif
                if(now_time - last_repeat_time == 2){ // 丢失的时间戳刚好是上一次重复的
                    time_buf[0] = (uint8_t)((last_repeat_time+1)>>24);
                    time_buf[1] = (uint8_t)((last_repeat_time+1)>>16);
                    time_buf[2] = (uint8_t)((last_repeat_time+1)>>8);
                    time_buf[3] = (uint8_t)(last_repeat_time+1);
                    #ifdef DEBUG
                    ESP_LOGW(RX_TASK_TAG, "丢失的时间戳刚好是上一次重复的，修正时间戳...");
                    num_rectified_losses += 1;
                    #endif
                    memcpy(target + 8,  time_buf, 4);  // 修正时间戳
                }
                last_losses_time = now_time-1;
            }
            time_buf[0] = (uint8_t)(nowtime>>24);
            time_buf[1] = (uint8_t)(nowtime>>16);
            time_buf[2] = (uint8_t)(nowtime>>8);
            time_buf[3] = (uint8_t)nowtime;
            memcpy(target + BAG_LEN,  start   , 1);
            target[BAG_LEN + 1] = sort;
            target[BAG_LEN + 2] = id_coodINBAG[0];
            target[BAG_LEN + 3] = id_coodINBAG[1];
            memcpy(target + BAG_LEN + 4,  data    , 4);
            memcpy(target + BAG_LEN + 8,  time_buf, 4);
            memcpy(target + BAG_LEN + 12, save_bit, 2);
            memcpy(target + BAG_LEN + 14, end,      2);
            num_bag_to_cood = 0;
            return_bag_num = 2;
            #ifdef DEBUG
            ESP_LOG_BUFFER_HEXDUMP("After fixing:", target, BAG_LEN*2, ESP_LOG_WARN);
            ESP_LOGW("TimeStamp", "Repeat time numbers: %d ; Losses time numbers: %d ; Rectified numbers : %d ", num_repeat_time, num_losses_time, num_rectified_losses + num_rectified_repeat);
            ESP_LOGW(RX_TASK_TAG, "---------------------------------------------------------------------------------------------");
            #endif
        }
        
        last_time = now_time;
        return return_bag_num;
    }else{
        // 其他类型数据包不需要排查时间戳重复或缺失的问题，按照传输帧进行正常打包
        if (sort == 0x0b) {
            time_buf[0] = (uint8_t)(boot_time>>24);
            time_buf[1] = (uint8_t)(boot_time>>16);
            time_buf[2] = (uint8_t)(boot_time>>8);
            time_buf[3] = (uint8_t)boot_time;
        } else {
            time_buf[0] = (uint8_t)(nowtime>>24);
            time_buf[1] = (uint8_t)(nowtime>>16);
            time_buf[2] = (uint8_t)(nowtime>>8);
            time_buf[3] = (uint8_t)nowtime;
        }
        
        memcpy(target + 0,  start   , 1);
        target[1] = sort;
        target[2] = id_coodINBAG[0];
        target[3] = id_coodINBAG[1];
        memcpy(target + 4,  data    , 4);
        memcpy(target + 8,  time_buf, 4);
        memcpy(target + 12, save_bit, 2);
        memcpy(target + 14, end,      2);
        return 1;
    }
}

// 1为处理好，0为解包失败
uint8_t tar_bag(uint8_t* row_data, int len, uint8_t* target)
{
    uint8_t sort = 0;
    // 错误输入
    if(len == 0 || row_data == NULL){
        return 0;
    }else{
        // 通讯协议： 0x5a 0xa5 xx xx xx xx 0x0d 0x0a
        if((*(row_data + 0) == 0x5a)  && (*(row_data + 6) == 0x0d) && (*(row_data + 7) == 0x0a))
        {
            if(*(row_data + 1) == 0xa5){
                sort = 0x01;  // 正常数据
                #ifdef DEBUG
                stm_data_bags += 1;
                #endif
            }else if(*(row_data + 1) == 0x01){
                #ifdef DEBUG
                stm_reset_times += 1;
                ESP_LOGE(RX_TASK_TAG, "--------------------------收到STM32重启数据包-------------------------------");
                #endif
                sort = 0x06;  // STM32重启
                all_config.stm_radar_failed = true;
                stm32Versions[0] = *(row_data + 3);
                stm32Versions[1] = *(row_data + 4);
                stm32Versions[2] = *(row_data + 5);
                if (all_config.ip_get && all_config.tcp_connect) {
                    reply_server(0x06, OTA_SUCCESS, stm32Versions);
                }
            }else if(*(row_data + 1) == 0x10){
                sort = 0x07;  // 雷达异常
                all_config.stm_radar_failed = true;
                #ifdef DEBUG
                stm_radar_failed_times += 1;
                ESP_LOGE(RX_TASK_TAG, "---------------------------收到雷达异常数据包--------------------------------");
                #endif
            }
            //符合协议
            all_config.rev_bag += 1;
            memcpy(target, row_data+2, 4);
            #ifdef DEBUG
            bytes_receive_from_stm = bytes_receive_from_stm + serial_len;
            // printf("%x %x %x %x",target[0],target[1],target[2],target[3]);
            #endif
        }
        return sort;
    }
}

// void get_stm32_state(void)
// {
//     uart_event_t event;
//     uint8_t send = CHECK_MODE;
//     // ESP_LOGI(RX_TASK_TAG, "start iap 1.");
//     xSemaphoreTake(MuxSem_UART_Handle, portMAX_DELAY);
//     // ESP_LOGI(RX_TASK_TAG, "start iap 2.");
//     // uart_flush(UART_NUM_1);
//     // xQueueReset(uart_event_queue);
//     int res = uart_write_bytes(UART_NUM_1, "m", 1);  // get
//     // ESP_LOGI(RX_TASK_TAG, "start iap 3. %d", res);
//     vTaskDelay(200 / portTICK_PERIOD_MS);
//     if (xQueueReceive(uart_event_queue, (void *)&event, 500 / portTICK_PERIOD_MS)) {
//         //判断类型
//         // ESP_LOGI(RX_TASK_TAG, "start iap 4.");
//         switch (event.type) 
//         {
//             case UART_DATA:
//                 {
//                     // ESP_LOGI(RX_TASK_TAG, "start iap 5.");
//                     int len_once_event = uart_read_bytes(UART_NUM_1, data_recive_bytes, RECEIVE_BUF_LEN, 50 / portTICK_RATE_MS);
//                     // ESP_LOGI(RX_TASK_TAG, "start iap 6.");
//                     if(len_once_event == 1){
//                         ESP_LOGI(RX_TASK_TAG, " Receive len: %d char: %d", len_once_event, data_recive_bytes[0]);
//                         if (data_recive_bytes[0] == MODE_B) {
//                             all_config.stm32_state = STM_RUNNING_BOOTLOADER;
//                             ESP_LOGI(RX_TASK_TAG, "STM32 is running Bootloader.");
//                         }  else if (data_recive_bytes[0] == MODE_D) {
//                             ESP_LOGI(RX_TASK_TAG, "STM32 is downloading upgrade bag.");
//                             all_config.stm32_state = STM_DOWNLOADING;
//                         } else if (data_recive_bytes[0] == MODE_C) {
//                             ESP_LOGI(RX_TASK_TAG, "Download is completed.");
//                             all_config.stm32_state = STM_DOWNLOADED;
//                         } else if (data_recive_bytes[0] == MODE_A) {
//                             ESP_LOGI(RX_TASK_TAG, "STM32 is running APP.");
//                             all_config.stm32_state = STM_RUNNING_APP;
//                         }
//                     } else {
//                         ESP_LOGI(RX_TASK_TAG, " Receive len: %d ", len_once_event);
//                         for(int i = 0; i < len_once_event; i++){
//                             printf(" %d ", data_recive_bytes[i]);
//                         }
//                         printf("\n");
//                     }
//                 }break;
//             default:
//                 {
//                     ESP_LOGI(RX_TASK_TAG, "STM32 is disconnected.");
//                     all_config.stm32_state = STM_DISCONNECT;
//                 }break;
//         }
//     } else {
//         ESP_LOGI(RX_TASK_TAG, "STM32 is disconnected.");
//         all_config.stm32_state = STM_DISCONNECT;
//     } 
//     xSemaphoreGive(MuxSem_UART_Handle); 
// }

// uint8_t send_order_to_stm32(uint8_t order)
// {
//     uart_event_t event;
//     uint8_t reply = 0, send = order;
    
//     xSemaphoreTake(MuxSem_UART_Handle, portMAX_DELAY);
//     // uart_flush(UART_NUM_1);
//     // xQueueReset(uart_event_queue);
//     uart_write_bytes(UART_NUM_1, &send, 1);  // 发送升级指令
//     vTaskDelay(200 / portTICK_PERIOD_MS);
//     if (xQueueReceive(uart_event_queue, (void *)&event, 200 / portTICK_PERIOD_MS)) {
//         //判断类型
//         switch (event.type) 
//         {
//             case UART_DATA:
//                 {
//                     int len_once_event = uart_read_bytes(UART_NUM_1, data_recive_bytes, RECEIVE_BUF_LEN, 50 / portTICK_RATE_MS);
//                     if (len_once_event == 1) {
//                         ESP_LOGI(RX_TASK_TAG, " Receive len: %d char: %d", len_once_event, data_recive_bytes[0]);
//                         reply = data_recive_bytes[0];
//                         if (reply == MODE_D) {
//                             all_config.stm32_state = STM_DOWNLOADING;
//                         }
//                     } else {
//                         ESP_LOGI(RX_TASK_TAG, " Receive len: %d ", len_once_event);
//                         for(int i = 0; i < len_once_event; i++){
//                             printf(" %d ", data_recive_bytes[i]);
//                         }
//                         printf("\n");
//                     }
//                 }break;
//             default:
//                 {
//                     ESP_LOGI(RX_TASK_TAG, "STM32 is disconnected.");
//                     all_config.stm32_state = STM_DISCONNECT;
//                 }break;
//         }
//     } else {
//         ESP_LOGI(RX_TASK_TAG, "STM32 is disconnected.");
//         all_config.stm32_state = STM_DISCONNECT;
//     } 
//     xSemaphoreGive(MuxSem_UART_Handle); 
//     return reply;
// }

// void abort_downloading_mode(uint8_t cancel)
// {
//     uint8_t order = cancel;
//     vTaskDelay(50 / portTICK_PERIOD_MS);
//     uart_write_bytes(UART_NUM_1, &order, 1);  // 发送中断下载指令
//     // vTaskDelay(50 / portTICK_PERIOD_MS);
//     uart_write_bytes(UART_NUM_1, &order, 1);  // 发送中断下载指令
//     vTaskDelay(50 / portTICK_PERIOD_MS);
// }


void rx_task(void *arg)
{
    // 三个空间的大小都是冗余设计
    
    // 用于装解包后的数据
    uint8_t data_out_tar[4] = {0};
    // 用于存cood后的数据
    uint8_t data_out_cood[BAG_LEN*2] = {0};
    // 用于存cood异常数据后的数据
    uint8_t info_out_cood[BAG_LEN] = {0};
    // 创建一个在txt忙时的存储空间
    uint8_t data_txt_busy[CODING_BUF_LEN] = {0};
    uint8_t ready_bag;  //data_out_cood 缓冲区修正好时间戳的包数

    // 事件类型变量
    uart_event_t event;
    int len_once_event = 0;
    int update_times = 0;

    while(1)
    {
        // if (all_config.tcp_connect && all_config.id_get && all_config.time_get && update_times < 100){
        //     if(all_config.stm32_state != STM_DOWNLOADING){
        //         time_t now = 0;
        //         time(&now);
        //         time_start = now;
        //         reply_server(0x0d, OTA_START, stm32Versions);
        //         start_stm32_iap();  // 开始IAP升级
        //         update_times++;
        //     }
        //     #ifdef DEBUG
        //     ESP_LOGI(RX_TASK_TAG, "downloading...... process: %d/100, success: %d/100, average_time:%d.", update_times, success_times, average_time);
        //     #endif
        //     vTaskDelay(5000 / portTICK_RATE_MS);
        // }else{
        //      ESP_LOGI(RX_TASK_TAG, "downloaded!!!!!! process: %d/100, success: %d/100, average_time:%d.", update_times, success_times, average_time);
        //     vTaskDelay(5000 / portTICK_RATE_MS);
        // }
        
        if (all_config.iap_start) { 
            if(all_config.stm32_state != STM_DOWNLOADING){
                reply_server(0x0d, OTA_START, stm32Versions);
                start_stm32_iap();  // 开始IAP升级
            }
            #ifdef DEBUG
            ESP_LOGI(RX_TASK_TAG, "downloading......");
            #endif
            vTaskDelay(5000 / portTICK_RATE_MS);
        } else { 
        // 正常接收STM32发来的体征数据
            // 一直堵塞等待串口事件的到来改成等待500ms没有数据就跳出！！！
            // xSemaphoreTake(MuxSem_UART_Handle, portMAX_DELAY);
            if (xQueueReceive(uart_event_queue, (void *)&event, 1000/portTICK_PERIOD_MS)) 
            {
                //判断类型
                // ESP_LOGI(RX_TASK_TAG, "--------------------------串口接收事件类型: %d --------------------------------", event.type);
                switch (event.type) 
                {
                    // UART_DATA：UART 收到数据事件。
                    // UART_BREAK：UART 收到 Break 信号事件。
                    // UART_BUFFER_FULL：UART 接收缓存满事件。
                    // UART_FIFO_OVF：UART FIFO 溢出事件。
                    // UART_FRAME_ERR：UART 接收帧错误事件。
                    // UART_PARITY_ERR：UART 接收奇偶校验错误事件。
                    // UART_DATA_BREAK：UART 发送数据并产生 Break 事件。
                    // UART_PATTERN_DET：UART 模式匹配事件。
                    // UART_EVENT_MAX：UART 事件类型最大值。
                    case UART_DATA:
                        {
                            //如果在100毫秒内没有收到新的数据，就读取字节数, 一定要读取，不然可能会溢出！
                            // ESP_LOGI(RX_TASK_TAG, "--------------------------串口传输协议帧内存--------------------------------");
                            // ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_1, (size_t*)&len_once_event));
                            len_once_event = uart_read_bytes(UART_NUM_1, data_recive_bytes, RECEIVE_BUF_LEN, 50 / portTICK_RATE_MS);
                            if (len_once_event < serial_len){
                                if(data_recive_bytes[0] == 0x18){
                                    uint8_t order = REBOOT;
                                    send_order_to_stm32(order);
                                }
                            }
                            #ifdef DEBUG
                            // ESP_LOGI(RX_TASK_TAG, " Rx FIFO   total:  %d   used: %d", RECEIVE_BUF_LEN, len_once_event);
                            // ESP_LOGI(RX_TASK_TAG, "--------------------------串口传输协议帧内存--------------------------------");
                            // ESP_LOG_BUFFER_HEXDUMP("Receive once in event", data_recive_bytes, len_once_event, ESP_LOG_INFO);
                            // ESP_LOGI(RX_TASK_TAG, "--------------------------------------------------------------------------");
                            // ESP_LOGI(RX_TASK_TAG, "等待cook的数据  %d\r\n", len_once_event/serial_len);
                            #endif
                            // for (int j = 0; j < len_once_event; j++){
                            //     printf("%c", data_recive_bytes[j]);
                            // }
                            // uart_write_bytes(UART_NUM_0, data_recive_bytes, len_once_event);

                            int loop = len_once_event / serial_len;
                            for (int i = 0; i < loop; i++)
                            {
                                uint8_t in_loop_recive[serial_len] = {0};
                                if(serial_len * (i+1) <= RECEIVE_BUF_LEN){
                                    memcpy(in_loop_recive, data_recive_bytes + serial_len * i , serial_len);
                                }else{
                                    ESP_LOGE(RX_TASK_TAG, "-------------------Rx buffer is not large enough! ------------------------");
                                }
                                // ESP_LOGI(RX_TASK_TAG, "--------------------------串口传输协议帧内存——in_loop_: %d --------------------------------",i);
                                // ESP_LOG_BUFFER_HEXDUMP("Receive once in event", in_loop_recive, serial_len, ESP_LOG_INFO);
                                // ESP_LOGI(RX_TASK_TAG, "-----------------------------------------------------------------------------------------");
                                //解包——成功后处理数据并且存入txt
                                uint8_t sort = tar_bag(in_loop_recive, 1, data_out_tar);
                                if(all_config.time_get && all_config.id_get) // 只有获取到世界时间 并且 获取到设备id号 的时候数据才有意义
                                {
                                    if(sort)
                                    {
                                        uint8_t save_bit[2] = {0xff, 0xff};
                                        if (sort == 0x01) {
                                            ready_bag = cood_data(data_out_tar, data_out_cood, save_bit, sort);
                                        } else {
                                            ready_bag = cood_data(data_out_tar, info_out_cood, save_bit, sort);
                                        }
                                        // ESP_LOGI(RX_TASK_TAG, "--------------------------WiFi传输协议帧内存--------------------------------");
                                        // ESP_LOG_BUFFER_HEXDUMP("COOD_DATA", data_out_cood,BAG_LEN, ESP_LOG_INFO);
                                        // ESP_LOGI(RX_TASK_TAG, "---------------------------------------------------------------------------");  
                                        
                                        //暂时存在内存里，等待fatfs_write完成了，才清除
                                        if (ready_bag > 0) {
                                            if(BAG_LEN * (num_busy+ready_bag) <= CODING_BUF_LEN) {
                                                num_busy += ready_bag;
                                                if (sort == 0x01) {
                                                    memcpy(data_txt_busy + (BAG_LEN * (num_busy-ready_bag)) , data_out_cood, BAG_LEN*ready_bag);
                                                } else {
                                                    memcpy(data_txt_busy + (BAG_LEN * (num_busy-ready_bag)) , info_out_cood, BAG_LEN*ready_bag);
                                                }
                                            } else {
                                                ESP_LOGE(RX_TASK_TAG, "-------------------Data_txt_busy buffer is not large enough! ------------------------");
                                            }
                                        }
                                        // ESP_LOGI(RX_TASK_TAG, "--------------------------ftfs协议帧内存--------------------------------");
                                        // ESP_LOG_BUFFER_HEXDUMP("COOD_DATA", data_txt_busy, 10, ESP_LOG_INFO);
                                        // ESP_LOGI(RX_TASK_TAG, "---------------------------------------------------------------------------");
                                    }
                                }
                            }
                            // 存入文件系统
                            if(num_busy >= TIME_OF_WRITE){
                                all_config.file_fine = fatfs_write(file_path , data_txt_busy , BAG_LEN * num_busy);
                                if (all_config.file_fine == 1) {  // 获取到文件权限并成功写入
                                    // ESP_LOGI(RX_TASK_TAG, "--------------------------------ftfs协议帧内存-------------------------------------");
                                    // ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data_txt_busy, BAG_LEN * num_busy, ESP_LOG_INFO);
                                    // ESP_LOGI(RX_TASK_TAG, "-----------------------------------------------------------------------------------");
                                    num_busy = 0;
                                    memset(data_txt_busy, 0, num_busy * BAG_LEN);
                                }else if (all_config.file_fine == 2) {  // 获取不到文件权限的情况，等待下次获取
                                    // ESP_LOGI(RX_TASK_TAG, "--------------------------- 暂时没有获取到文件限权 --------------------------------");
                                    if (num_busy >= CODING_BUF_LEN/BAG_LEN*3/4) {
                                        if (all_config.file_fine != 3) {
                                            all_config.someone_changed = true;
                                        }
                                        all_config.file_fine = 3;     // 代表待写入文件缓冲区快满了，TCP发送那里需要暂停发送进程，释放TXT限权
                                        ESP_LOGW(RX_TASK_TAG, "Rx buffer is nearly full, num_busy: %d", num_busy);
                                    }
                                } else if(all_config.file_fine == 0) {
                                    ESP_LOGE(RX_TASK_TAG, "文件损坏，格式化并重启!");
                                    esp_restart();
                                } else if(all_config.file_fine == 4) {
                                    ESP_LOGW(RX_TASK_TAG, "文件已满，已清空文件数据!");
                                }
                            }
                            
                        }break;
                    case UART_FIFO_OVF:
                        break;
                    case UART_BUFFER_FULL:
                        break;
                    case UART_PARITY_ERR:
                        break;
                    case UART_FRAME_ERR:
                        break;
                    case UART_BREAK:
                        break;
                    case UART_EVENT_MAX:
                        break;
                    case UART_PATTERN_DET:
                        break;
                    case UART_DATA_BREAK:
                        break;
                    default:
                        break;
                }
            }
            // xSemaphoreGive(MuxSem_UART_Handle); 
        }
    
    }
    
}