#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "all_config.h"
#include "save/include/save.h"
#include "serial/include/serial.h"
#include "ota/include/simple_ota_example.h"

#define CONFIG_EXAMPLE_IPV4 1
#define CONFIG_EXAMPLE_IPV4_ADDR IPV4_ADDR      // 服务器IP地址
#define CONFIG_EXAMPLE_PORT PORT_MY             // 服务器监听端口
#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR
#define PORT CONFIG_EXAMPLE_PORT
#define STRESS_TEST_BAG_NUM 60                  // 压力测试包的包数

extern const uint8_t Versions[3];               // 程序版本号
extern uint8_t stm32Versions[3];          // STM32程序版本号
extern uint8_t mac[6];                          // 存放此ESP32的mac物理地址
extern uint8_t id_get[2];                       // 存放服务器分配的设备id号
extern All_Flags all_config;                    // 系统运行状态标志位
extern SemaphoreHandle_t MuxSem_TXT_Handle;     // TXT文件信号量
extern SemaphoreHandle_t MuxSem_TCP_Handle;     // TCP发送信号量
extern TaskHandle_t stay_for_check_wifi_for_server_handle;     // 获取wifi强度的任务控制句柄
extern TaskHandle_t stay_for_stress_test_for_server_handle;    // 网络传输压力测试任务控制句柄
extern TaskHandle_t ReadFile_TCP_handle;                       // 读取文件并发送的任务控制句柄
extern TaskHandle_t OTA_handle;                                // OTA（远程固件升级）控制句柄

static const char *TAG = "TCP";
extern int last_timestamp;                      // 最后一个入表的时间戳
extern u16_t num_busy;                          // 待写入文件的包数
u32_t disconnection_times = 0;                  // 尝试连接连续失败的次数（在连接上wifi却持续连接失败的情况下则考虑重启设备）
int sock = -1;                                  // 建立socket分配的sockid
char rx_buffer[LEN_OF_REC];                     // TCP接收数据解包缓存区
const uint8_t file_broken[BAG_LEN] = {0x5a, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0a};// 文件系统损坏数据包
uint8_t ota_reply = 0x00;
uint8_t iap_reply = 0x00;

#ifdef DEBUG
u32_t reconnect_times = 0;                        // 断线重连次数
u32_t recv_err_times = 0;                         // 接收监听出错次数
#endif

/// @brief 回复服务器，固定长度的数据包，具体包类型含义参考：【金山文档】 通讯协议V1.0-2023-3-7 https://kdocs.cn/l/cehqTbqdYrgt
/// @param sort 包类型
/// @param p_data 指向数据帧的首地址
/// @param p_time 指向时间戳帧的首地址
/// @param p_leisure 指向空闲/留用位的首地址
// int reply_server(uint8_t sort, uint8_t *id, void *p_data, void *p_time, void *p_leisure)
// {
//     uint8_t reply_bag[BAG_LEN] = {0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0a};
//     uint8_t *p;
//     int i;

//     reply_bag[1] = sort;
//     reply_bag[2] = id[0];
//     reply_bag[3] = id[1];

//     if (p_data != NULL) {
//         p = (uint8_t *)p_data;
//         for ( i = 0; i < 4; i++ ) {
//             reply_bag[4+i] = p[i];
//         }
//     }
//     if (p_time != NULL) {
//         p = (uint8_t *)p_time;
//         for ( i = 0; i < 4; i++ ) {
//             reply_bag[8+i] = p[i];
//         }
//     }
//     if (p_leisure != NULL) {
//         p = (uint8_t *)p_leisure;
//         for ( i = 0; i < 2; i++ ) {
//             reply_bag[12+i] = p[i];
//         }
//     }
    
// }


// sort 05 是获取id，08是回应
uint8_t send_mac(uint8_t sort, const uint8_t * mac_will_send)
{
    uint8_t tar_get[BAG_LEN] = {0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0a};
    tar_get[1] = sort;
    tar_get[2] = id_get[0];
    tar_get[3] = id_get[1];
    tar_get[4] = mac_will_send[0];
    tar_get[5] = mac_will_send[1];
    tar_get[6] = mac_will_send[2];
    tar_get[7] = mac_will_send[3];
    tar_get[12] = mac_will_send[4];
    tar_get[13] = mac_will_send[5];
    // 首次发送，需要一个 
    int err = 0;
    if (sock < 0){
        ESP_LOGE(TAG, "socket 未连接，请自查socket");
        return 0;
    }else{
        xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);//堵塞等待可写
        // ESP_LOGI(TAG, "TCP发送MAC或心跳包");
        err = send(sock, tar_get, BAG_LEN, 0);
        xSemaphoreGive(MuxSem_TCP_Handle);
        // ESP_LOGI(TAG, "释放TCP");
    }
    if (err == BAG_LEN){
        if(sort == 0x05){
            ESP_LOGW(TAG, "mac发送成功.");
        }else if(sort == 0x08){
            ESP_LOGI(TAG, "心跳包发送成功. ");
        }
    } else {
        ESP_LOGE(TAG, "TCP发送MAC或心跳包失败: errno %d", errno);
        return 0;
    }
    
    return 1;
}


uint8_t rev_cood()
{
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket 不正常, 请自查socket");
        return 0;
    }
    // 堵塞监听，如果正常接收就发送mac作为回应
    int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
    // 1. 返回值大于0： 成功执行，返回接收到的字节数。
    // 2. 返回值等于0： 对端已关闭连接。
    // 3. 返回值等于-1：连接出现错误，错误码errno
    if (len == 0) {
        // 对端socket调用close()关闭
        ESP_LOGE(TAG, "recv failed: errno %d", errno);
        return 0;
    } else if (len == -1) {
        // 错误码errno取值以及对应发生的错误：
        // EINTR 4：操作被信号中断，通常情况下可以重新调用该函数。
        // EAGAIN 11：套接字已标记为非阻塞，而接收操作被阻塞或者接收超时 
        // EWOULDBLOCK 140: 本来应该阻塞，却没有阻塞
        // EWOULDBLOCK或EAGAIN：接收数据时没有立即可用的数据或缓冲区已满。当使用非阻塞套接字时可能会出现这种情况。

        // EBADF：sock不是有效的描述词 
        // ECONNREFUSE：远程主机阻绝网络连接 
        // EFAULT：内存空间访问出错 
        // EINVAL：参数无效 
        // ENOMEM：内存不足 
        // ENOTCONN：与面向连接关联的套接字尚未被连接上 
        // ENOTSOCK 128：sock索引的不是套接字 当返回值是0时，为正常关闭连接；
        // EISCONN 113: 套接口已经建立连接，而且第一次连接是成功的
        // EBADMSG 104: Not a data message 不是数据类型消息
        if (errno == EINTR ||errno == EAGAIN ||errno == EWOULDBLOCK){
            ESP_LOGW(TAG, "recv errno : %d", errno);
            return 1;
        }else{
            // 除了以上三种错误码能接受，其他错误均断开socket重新连接
            ESP_LOGE(TAG, "recv errno : %d", errno);
            return 0;
        }
    } else if(len <= BAG_REC_LEN*2) {
        // 将接受到的数据转换成字符串
        uint8_t rx_2_string[LEN_OF_REC+1];
        char host_ip[] = HOST_IP_ADDR;
        rx_2_string[len] = 0;
        // 作为末尾
        memcpy(rx_2_string, rx_buffer, len);
        ESP_LOGW(TAG, "Received %d bytes from %s:", len, host_ip);
        ESP_LOGW(TAG, "%s", rx_2_string);

        // 解包程序
        ESP_LOGW(TAG, "--------------------------TCP接收协议帧内存------------------------------------------------------------");
        ESP_LOG_BUFFER_HEXDUMP("TCP Receive once in event", rx_2_string, len, ESP_LOG_WARN);

        // 先判断包的完整性，如果不完整就返回一个包不完整要求重发的mac，但是标志位为09
        int bag_num = 0;
        if (!(len % BAG_REC_LEN))
        {
            // 计算有多少个命令包
            bag_num = len / BAG_REC_LEN;                
            // 严格判断，不允许错序和意外多发
            // 接收到的命令按照最新的执行
            for (int i = 0; i < bag_num; i++)
            {
                ESP_LOGW(TAG, "--------------------------收到命令_开始解包--------------------------------");

                if (rx_2_string[0 + i*BAG_REC_LEN] == 0x5a && rx_2_string[14+i*BAG_REC_LEN] == 0x0d && rx_2_string[15+i*BAG_REC_LEN] == 0x0a)
                {
                    if(rx_2_string[1 + i*BAG_REC_LEN] == 0x12) {
                        // 压力测试
                        ESP_LOGW(TAG, "--------------------------收到网络压力测试命令--------------------------------");
                        // 给出信号量，开始干活就行
                        xTaskNotifyGive(stay_for_stress_test_for_server_handle);
                    } else if (rx_2_string[1 + i*BAG_REC_LEN] == 0x10) {
                        // 强度自查
                        ESP_LOGW(TAG, "--------------------------收到自查WiFi强度命令--------------------------------");
                        // 给出信号量，开始干活就行
                        xTaskNotifyGive(stay_for_check_wifi_for_server_handle);
                    } else if (rx_2_string[1 + i*BAG_REC_LEN] == 0x11) {
                        // 获取的是id
                        ESP_LOGW(TAG, "--------------------------收到自查ID回复-----------------------------------");
                        id_get[0] = rx_2_string[2+ i*BAG_REC_LEN];
                        id_get[1] = rx_2_string[3+ i*BAG_REC_LEN];
                        all_config.id_get = true;
                        ESP_LOGW(TAG, "--------------------------收到ID  %x %x -------------------------------------", id_get[0], id_get[1]);
                        last_timestamp = (rx_2_string[8+ i*BAG_REC_LEN] << 24) + (rx_2_string[9+ i*BAG_REC_LEN] << 16) + (rx_2_string[10+ i*BAG_REC_LEN] << 8) + rx_2_string[11+ i*BAG_REC_LEN];
                        ESP_LOGW(TAG, "--------------------------最后入表的时间戳：%d --------------------", last_timestamp);
                        all_config.receive_last_timestamp = true;
                        all_config.someone_changed = true;
                        xTaskNotifyGive(ReadFile_TCP_handle); // 收到ID后才执行发送任务，不然可能会出错
                        // 收到服务器那边的发过来的数据包才算新一次重连成功
                    } else if (rx_2_string[1 + i*BAG_REC_LEN] == 0x13) {
                        // 服务器请求更新固件
                        
                        uint8_t lastVersions[3];
                        lastVersions[0] = rx_2_string[5 + i*BAG_REC_LEN];
                        lastVersions[1] = rx_2_string[6 + i*BAG_REC_LEN];
                        lastVersions[2] = rx_2_string[7 + i*BAG_REC_LEN];
                        uint32_t last_versions = lastVersions[0]*256*256 + lastVersions[1]*256 + lastVersions[2];
                        uint32_t versions = Versions[0]*256*256 + Versions[1]*256 + Versions[2];
                        ESP_LOGW(TAG, "收到更新固件请求, 当前版本：%d.%d.%d, 最新版本号: %d.%d.%d.", Versions[0], Versions[1], Versions[2], lastVersions[0], lastVersions[1], lastVersions[2]);
                        if (last_versions > versions) {
                            ota_reply = OTA_START;  // 告诉服务器已经收到升级指令，并且现在的版本确实比最新版本低
                            // 执行OTA升级任务
                            all_config.ota_start = true;
                            all_config.someone_changed = true;
                            xTaskNotifyGive(OTA_handle);
                        } else {
                            ota_reply = OTA_LASTED;  // 告诉服务器已经收到升级指令，但是现在的版本比最新版本高或一样，无需升级固件
                            reply_server(0x0c, OTA_LASTED, Versions);
                            ESP_LOGW(TAG, "已是最新版本，不用升级");
                        }
                        
                    } else if (rx_2_string[1 + i*BAG_REC_LEN] == 0x14) {
                        // 服务器请求更新STM32固件
                        uint8_t lastVersions[3];
                        lastVersions[0] = rx_2_string[5 + i*BAG_REC_LEN];
                        lastVersions[1] = rx_2_string[6 + i*BAG_REC_LEN];
                        lastVersions[2] = rx_2_string[7 + i*BAG_REC_LEN];
                        uint32_t last_versions = lastVersions[0]*256*256 + lastVersions[1]*256 + lastVersions[2];
                        uint32_t versions = stm32Versions[0]*256*256 + stm32Versions[1]*256 + stm32Versions[2];
                        ESP_LOGW(TAG, "收到更新STM32固件请求, 当前版本：%d.%d.%d, 最新版本号: %d.%d.%d.", stm32Versions[0], stm32Versions[1], stm32Versions[2], lastVersions[0], lastVersions[1], lastVersions[2]);
                        if (last_versions > versions) {
                            iap_reply = OTA_START;  // 告诉服务器已经收到升级指令，并且现在的版本确实比最新版本低
                            // 执行OTA升级任务
                            all_config.iap_start = true;
                            all_config.someone_changed = true;
                        } else {
                            iap_reply = OTA_LASTED;  // 告诉服务器已经收到升级指令，但是现在的版本比最新版本高或一样，无需升级固件
                            reply_server(0x0d, OTA_LASTED, stm32Versions);
                            ESP_LOGW(TAG, "已是最新版本，不用升级");
                        }
                        
                    } 
                }
            }
        }
        ESP_LOGW(TAG, "-------------------------------------------------------------------------------------------------------");
    }
    return 1;
}

void tcp_client_task(void *pvParameters)
{
    // 用于存储
    char host_ip[] = HOST_IP_ADDR;

    // 用于存储套接字地址族，通常为 AF_INET（IPv4）或 AF_INET6（IPv6）
    int addr_family = 0;
    int ip_protocol = 0;
    
    // sin_family：地址族，通常设置为AF_INET，表示IPv4地址。
    // sin_port：端口号，使用网络字节序（big-endian）表示。
    // sin_addr：IP地址，使用in_addr类型表示，可以通过inet_addr()等函数将字符串类型的IP地址转换为in_addr类型。
    // sin_zero：填充字段，通常设置为0。
    struct sockaddr_in dest_addr;
    // 将字符串形式的IPv4地址转换成整数型的IP地址
    dest_addr.sin_addr.s_addr = inet_addr(host_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    addr_family = AF_INET;

    // addr_family指定了地址族，常用的有AF_INET和AF_INET6，分别表示IPv4和IPv6。
    // SOCK_STREAM指定了传输协议，常用的有SOCK_STREAM和SOCK_DGRAM，分别表示面向连接的TCP协议和无连接的UDP协议。
    // ip_protocol指定了协议族，常用的有IPPROTO_TCP和IPPROTO_UDP，分别表示TCP和UDP协议。
    
    while (1) 
    {
        if(all_config.ip_get)
        {
            ESP_LOGI(TAG, "—————————————————————————————————已获取到IP地址, 建立socket链接—————————————————————————————————————————");
            sock = socket(addr_family, SOCK_STREAM, ip_protocol);
            if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            }else{
                // // 设置非阻塞模式
                // fcntl(sock, F_SETFL, O_NONBLOCK);
                // int flags = fcntl(sock, F_GETFL, 0);
                // fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                // 默认为堵塞模式
                struct timeval tv;
                tv.tv_sec = TIMEOT_OF_SEND; 
                tv.tv_usec = 0;
                int ret_snd = 1;
                // int ret_rev = 1;
                // ret_rev = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                // 设置发送超时时间为30秒
                ret_snd = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)); 
                if (ret_snd < 0) {
                    // 设置超时时间失败
                    perror("setsockopt error");
                }
                else{
                    ESP_LOGI(TAG, "Socket created and set not blocked in rec, connecting to %s:%d", host_ip, PORT);
                    // 连接到远程主机
                    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in6));
                    // ESP_LOGI(TAG, "connect err: %d", err);
                    if (err != 0) {
                        ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
                        // 重新创建socket进行链接
                    }else {
                        // 链接成功
                        ESP_LOGI(TAG, "Socket %d successfully connected", sock);
                        all_config.tcp_connect = true;
                        if(!all_config.reboot_fisrt_time){
                            all_config.reconnect_fisrt_time = true;
                        }
                        // -------------------------获取ID--------------------------------
                        ESP_LOGI(TAG, "--------------------------MAC-C3--------------------------------");
                        ESP_LOG_BUFFER_HEXDUMP("Mac will be sent : ", mac, 6, ESP_LOG_INFO);
                        ESP_LOGI(TAG, "----------------------------------------------------------------");
                        all_config.receive_last_timestamp = false;
                        all_config.someone_changed = true;
                        if(send_mac(0x05, mac))
                        {
                            disconnection_times = 0;
                            while (1)
                            {
                                ESP_LOGI(TAG, "—————————————————————————————————Socket开启监听————————————————————————————————————————");
                                if(!rev_cood()){ 
                                    ESP_LOGE(TAG, "rev_cood() return 0.");  // 一直堵塞在这监听，直到收到服务器的指令
                                    #ifdef DEBUG
                                    recv_err_times += 1;
                                    #endif
                                    break;
                                }
                                vTaskDelay(1000 / portTICK_RATE_MS);
                            }
                        }
                    }
                }
            }
            // 不延时，主动关闭socket，不允许这边继续发未发完的数据
            all_config.tcp_connect = false;
            all_config.someone_changed = true;
            
            // 如果创建过sock，就关掉再重启 
            if (sock != -1) {     
                ESP_LOGE(TAG, "Shutting down socket and restarting...");
                shutdown(sock, 0);
                close(sock);
                sock = -1;
                #ifdef DEBUG
                reconnect_times += 1;
                #endif
                disconnection_times ++;
                if(disconnection_times > 36000){
                    // 连续一个小时没有连接上网络/服务器，就重启试试     
                    // 堵塞获取各任务信号量限权，等待tcp、txt等任务完成当前操作再重启，避免TCP发送失败、文件数据出错
                    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
                    xSemaphoreTake(MuxSem_TXT_Handle, portMAX_DELAY);
                    xSemaphoreGive(MuxSem_TXT_Handle);
                    ESP_LOGE(TAG, "Disconnection times is too much! Try to reset esp32...");
                    esp_restart();
                }
            }
            // 等待服务器那边关闭socket,这样子以确保不会出现重叠线程导致数据重复发送的问题
            vTaskDelay(TIME_OF_RECONNECT*1000 / portTICK_PERIOD_MS);
        } else {
            // ESP_LOGE(TAG, "all_config.ip_get == 0");
            vTaskDelay(1000 / portTICK_PERIOD_MS);  
        }  
    }
}


void keep_connect(void *pvParameters)
{
    uint16_t i = 0;
    while (1)
    {
        i++;
        #ifdef DEBUG
        if(i % 60 == 0) {
                char task_in[1024 * 2];
                vTaskList((char*)task_in);
                printf("------------------System tasks status-----------------\n");
                printf("Versions %d.%d.%d\n", Versions[0], Versions[1], Versions[2]);
                printf("task_name   task_state  priority  stack   task_num\n");
                printf("%s",task_in);
                printf("------------------System tasks status-----------------\n"); 
        }
        #endif
        if(i % TIME_OF_CRY == 0 && !all_config.ota_start){
            if ( all_config.ip_get && all_config.tcp_connect && all_config.time_get ) {
                if (!all_config.id_get){  
                    // reboot后没有获取到最后一次入表时间戳则一直请求
                    all_config.receive_last_timestamp = false;
                    all_config.someone_changed = true;
                    send_mac(0x05, mac);
                } else {
                    #ifdef DEBUG
                    time_t now = 0;
                    struct tm timeinfo = { 0 };
                    char strftime_buf[64];
                    time(&now);
                    localtime_r(&now, &timeinfo);
                    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
                    ESP_LOGI("Time", " %s ; %d ; Data to be written to file: %5d s , %5d bytes. ", strftime_buf, now, num_busy, num_busy*BAG_LEN);
                    #endif
                    send_mac(0x08, mac);
                }   
            }
        }
        if(i % 3600 == 0 && !all_config.ota_start){   
            i = 0;
            if (all_config.ip_get && all_config.tcp_connect && all_config.file_fine == 0 ) {
                // 如果有网络就发送文件损坏数据包给服务器告知用户
                xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
                ESP_LOGW(TAG, "获取TCP 发送文件损坏数据包");
                int err = send(sock, file_broken, BAG_LEN, 0);
                xSemaphoreGive(MuxSem_TCP_Handle);
                ESP_LOGW(TAG, "释放TCP ");
                if(err == BAG_LEN){
                    ESP_LOGW(TAG, "发送成功。");
                }else{
                    ESP_LOGE(TAG, "发送失败。");
                }
            }
        }
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

uint8_t get_rssi()
{
  wifi_ap_record_t info;
  esp_err_t error = esp_wifi_sta_get_ap_info(&info);

  if (error == ESP_OK) {
    return info.rssi;
  }

  return 0; // If an error occurs, return -1 to signal the calling function.
}

void stay_for_check_wifi_for_server(void *pvParameters)
{
    while (1)
    {
        // 堵塞，等待有check命令
        ulTaskNotifyTake( pdTRUE, portMAX_DELAY);

        // 假设检测到的WiFi信号强度是：150
        uint8_t ST_WIFI = 0;
        uint8_t target_buf[BAG_LEN] = {0x5a, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x0d, 0x0a};
        target_buf[2] = id_get[0];
        target_buf[3] = id_get[1];
        int err = 0;
        if (sock < 0){
            ESP_LOGE(TAG, "socket 不正常，请自查socket");
        } else {
            xSemaphoreTake(MuxSem_TCP_Handle,portMAX_DELAY);//堵塞等待可写
            ESP_LOGW(TAG, "获取TCP 4 —— Send WIFI ST");
            ST_WIFI = get_rssi();
            target_buf[7] = ST_WIFI;
            err = send(sock, target_buf, BAG_LEN, 0);
            xSemaphoreGive(MuxSem_TCP_Handle);
            ESP_LOGW(TAG, "释放TCP 4 —— Send WIFI ST");
            if (err == BAG_LEN){
                ESP_LOGW(TAG, "强度发送成功");
            }
            else{
                ESP_LOGE(TAG, "KEEP————Error occurred during sending: errno %d", errno); 
            }
        }
        
        // 休息一下
        vTaskDelay((TIME_OF_CRY * 1000) / portTICK_RATE_MS);
    }
}

void stay_for_stress_test_for_server(void *pvParameters)
{
    uint8_t data_cpy[BAG_LEN*STRESS_TEST_BAG_NUM] = {0}; // 压力测试包总包
    uint8_t data_model[BAG_LEN] = {0}; // 压力测试包的格式模型
    uint8_t data_bag_num[4] = {0}; // 压力测试包的总包数
    uint8_t save_bit[2] = {0xff,0xff};
    int send_times = 10, sum_bag = send_times*STRESS_TEST_BAG_NUM;
    uint8_t *pointer = (uint8_t *)&sum_bag;

    for(int i = 0; i < 4; i++){
        data_bag_num[i] = pointer[i];
    }
    cood_data(data_bag_num, data_model, save_bit, 0x09); // 压力测试包的标识位为0x09
    for (int i = 0; i < STRESS_TEST_BAG_NUM; i++){
        memcpy(&data_cpy[i*BAG_LEN], data_model, BAG_LEN);
    }

    while (1)
    {
        // 堵塞，等待有check命令
        ulTaskNotifyTake( pdTRUE, portMAX_DELAY);
       
        int err = 0;
        if (sock < 0) {
            ESP_LOGE(TAG, "socket 不正常，请自查socket");
        } else {
            for(int i = 0; i < send_times; i++) {
                xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);//堵塞等待可写
                ESP_LOGW(TAG, "获取TCP 5 —— Send STRESS TEST"); 
                err = send(sock, data_cpy, BAG_LEN*STRESS_TEST_BAG_NUM, 0);
                xSemaphoreGive(MuxSem_TCP_Handle);
                ESP_LOGW(TAG, "释放TCP 5 —— Send STRESS TEST");   
                if (err == BAG_LEN*STRESS_TEST_BAG_NUM)
                    ESP_LOGW(TAG, "第%d个压力测试包发送成功", i+1);
                else{
                    ESP_LOGE(TAG, "KEEP————Error occurred during sending: errno %d", errno);
                }
                vTaskDelay(300 / portTICK_RATE_MS);
            }
        }
        // 休息一下
        vTaskDelay((TIME_OF_CRY * 1000) / portTICK_RATE_MS);
        // all_config.iap_start = 1;
    }
}