#include <stdio.h>
#include "smart_config.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "all_config.h"
#include "lwip/sockets.h"

///事件组句柄
extern EventGroupHandle_t s_wifi_event_group;

///smartconfig锁句柄
extern SemaphoreHandle_t MuxSem_SMART_Handle;
extern TaskHandle_t Smart_task_handle;
extern SemaphoreHandle_t MuxSem_TCP_Handle;
extern All_Flags all_config;
extern SemaphoreHandle_t MuxSem_TXT_Handle;

//nvs句柄
nvs_handle my_HandleNvs;

//smart标志位
static const int CONNECTED_BIT = BIT0;
// static const int ESPTOUCH_DONE_BIT = BIT1;
static const int PRESS_KEY_BIT = BIT2;

static const char *TAG = "smartconfig";

//nvs成功标志位
uint8_t nvs_success = 0;
uint8_t mac[6];   // 存放此ESP32的mac物理地址
bool smartconfig_successfully = false;

void smartconfig_example_task(void * parm);

/// @brief 尝试nvs内部进行链接，如果失败0|成功1
/// @return 
uint32_t Get_Wifi_NVS()
{
    //检查nvs中有多少账号密码————————————————暂时没写
    int acount_num = 1;//假装检测到只有一个账号密码
    if(acount_num == 0)
        return 0;

    printf("Have %d Acount\n",acount_num);

    char wifi_ssid[33] = { 0 };
    char wifi_passwd[65] = { 0 };
    size_t len;

    // 打开存储空间，查看账号密码
    int err = nvs_open("WiFi_cfg", NVS_READWRITE, &my_HandleNvs);
    if (err != ESP_OK) 
    {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return 0;
    }
    else
    {
        // 用于组合字符串的缓冲区
        char wifi_ssid_buf[100];
        char wifi_passwd_buf[100];
        uint32_t acount_cur = 1;

        char* wifi_ssid_name_1 = "wifi_ssid";
        char* wifi_ssid_passwd_1 = "wifi_passwd";

        while(acount_cur <= acount_num)
        {
            printf("Finding the ssid and password : %d\n",acount_cur);
            // 组合字符串用于索寻账号密码
            sprintf(wifi_ssid_buf,"%s%d",wifi_ssid_name_1,acount_cur);
            sprintf(wifi_passwd_buf,"%s%d",wifi_ssid_passwd_1,acount_cur);

            // 键值对取账号
            len = sizeof(wifi_ssid);
            err = nvs_get_str(my_HandleNvs, wifi_ssid_buf, wifi_ssid, &len);
            if(err == ESP_OK)
                printf("%s get ok :%s  \n", wifi_ssid_buf, wifi_ssid);
            else 
                printf("%s get err \n", wifi_ssid_buf);
            // 键值对取密码
            len = sizeof(wifi_passwd);
            err = nvs_get_str(my_HandleNvs, wifi_passwd_buf, wifi_passwd, &len);
            if(err == ESP_OK)
                printf("%s get ok : %s \n",wifi_passwd_buf, wifi_passwd);
            else 
                printf("%s get err \n",wifi_passwd_buf);


            /* 提交*/
            err = nvs_commit(my_HandleNvs);
            nvs_close(my_HandleNvs);

            // 根据获取到的进行配置WiFi和链接
            // 释放和获取资源刷新WiFi
            esp_wifi_stop();
            esp_wifi_start();
            wifi_config_t wifi_config;
            bzero(&wifi_config, sizeof(wifi_config_t));
            memcpy(wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, wifi_passwd, sizeof(wifi_config.sta.password));
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
            ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
            esp_wifi_connect();
            //等待十秒——在时间组轮询里面会把标志位安排的明明白白的
            vTaskDelay(10000 / portTICK_PERIOD_MS);

            acount_cur++;
            //检查一下标志位_这个标志位会随着WiFi链接成功而置1_如果是此处nvs账号没有搞定，就打开smartconfig
            if(nvs_success == 1){
                return 1;
            }
                
        }
        return 0;
    }
}

/// @brief 在每次smartconfig后都对新来的进行存储
/// @return 
void Set_Wifi_NVS(const char * acount,const char * password)
{
    //检查nvs中有多少账号密码————————————————暂时没写
    int acount_num = 0;
    // 假装每次进配网都是新机子
    printf("Have %d Acount\n",acount_num);
    /* 打开一个NVS命名空间 */
    esp_err_t err = nvs_open("WiFi_cfg", NVS_READWRITE, &my_HandleNvs);
    if (err != ESP_OK) 
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    else 
    {
        printf("NVS can be writen\n");
        char * wifi_ssid_name = "wifi_ssid";
        char * wifi_passwd_name = "wifi_passwd";

        // 用于组合索引名字的缓存
        char wifi_ssid_buf[100];
        char wifi_passwd_buf[100];

        sprintf(wifi_ssid_buf,"%s%d",wifi_ssid_name,acount_num+1);
        sprintf(wifi_passwd_buf,"%s%d",wifi_passwd_name,acount_num+1);

        // 账号字符串写入
        err = nvs_set_str(my_HandleNvs,wifi_ssid_buf,(const char *)acount);
        if(err == ESP_OK)
            printf("%s : %s set ok \n",wifi_ssid_buf,acount);
        else 
            printf("%s set err \n",wifi_ssid_buf);
        // 密码字符串写入 
        err = nvs_set_str(my_HandleNvs,wifi_passwd_buf,(const char *)password);
        if(err == ESP_OK)
            printf("%s : %s set ok \n",wifi_passwd_buf,password);
        else 
            printf("%s set err \n",wifi_passwd_buf);

        /* 提交*/
        err = nvs_commit(my_HandleNvs);
    }
    nvs_close(my_HandleNvs);
}

// uint8_t Got_And_Save()

// 事件回调组，当事件队列里面有东西，并且被时间循环检索到了，就统一进到这里
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // 在调用esp_wifi_start()后，如果Wi-Fi芯片成功地启动并与接入点连接成功，则WIFI_EVENT_STA_START事件将被触发    
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 例如Wi-Fi信号丢失、接入点断电、密码错误等。当此事件被触发时，通常需要重新连接Wi-Fi
        ESP_LOGE(TAG, "WiFi disconnected!");
        if (all_config.ip_get){
            all_config.someone_changed = true;
        }
        all_config.ip_get = false;
        all_config.wifi_connect = false;
        if(!all_config.smart_start){
            esp_wifi_connect();
            xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 正确获取到ip 意味着 如果是初次进来，可以进行nvs的存储
        nvs_success = 1;//nvs_connect_success
        if (!all_config.ip_get){
            all_config.someone_changed = true;
        }
        all_config.ip_get = true;
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        //如果是进入过smartconfig，就会有sc_event的发生，将进入下列
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
        //如果是进入到获取到pswd和ssid，意味着配网成功，配网成功，覆盖最新的
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        //判断好了类型，直接能判断这就是smartconfig_event的账号密码的data，直接强制转换 
        // ssid：Wi-Fi SSID 的数组。
        // password：Wi-Fi 密码的数组。
        // bssid_set：指示是否设置了 Wi-Fi BSSID 的布尔值。
        // bssid：Wi-Fi BSSID 的数组。
        // type：SmartConfig 类型的枚举值。目前 ESP32 支持两种类型：ESPTOUCH 和 AIRKISS。
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;

        // wifi_config_t 结构体定义了 Wi-Fi 网络配置信息，它包括以下成员：
        // wifi_mode_t mode：Wi-Fi 工作模式，可以是 WIFI_MODE_AP（热点模式）或 WIFI_MODE_STA（客户端模式）。
        // uint8_t sta[6]：Wi-Fi 客户端的 MAC 地址。
        // wifi_sta_config_t sta：Wi-Fi 客户端模式下的配置信息，包括 SSID、密码和 BSSID 等。
        // wifi_ap_config_t ap：Wi-Fi 热点模式下的配置信息，包括 SSID、密码和信道等。
        // wifi_country_t country：Wi-Fi 国家信息，用于指定可用信道和功率限制等。
        // uint8_t sleep_type：Wi-Fi 休眠类型，可以是 WIFI_PS_NONE（不休眠）、WIFI_PS_MIN_MODEM（仅关闭 modem 电源）或 WIFI_PS_MAX_MODEM（关
        wifi_config_t wifi_config;

        // 设置三个缓冲区
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };
        uint8_t rvd_data[33] = { 0 };

        // 将wifi的配置表清空
        bzero(&wifi_config, sizeof(wifi_config_t));
        // 将接受到的账号密码保存到配置表中————sta中ssid位数为32个，如果ssid传入的大于32，幅值32会溢出
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

        // BSSID代表“基本服务集标识符”，是一个无线网络的唯一标识符，由6个字节的MAC地址组成。在一个大型的WiFi网络中，可能有多个无线接入点（AP），每个AP都有一个唯一的BSSID
        // 用来区分不同的AP。客户端设备可以使用BSSID连接到一个特定的AP，这可以用于提高连接的稳定性和性能，以及定位设备在网络中的位置。
        wifi_config.sta.bssid_set = evt->bssid_set;

        // 如果有用到bssid
        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(password, evt->password, sizeof(wifi_config.sta.password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);

        // 存入到nvs
        Set_Wifi_NVS((char*)ssid,(char*)password);
        ESP_LOGI(TAG, "Save ssid and password successfully.");

        // 判断是哪个手机软件搞来的
        if (evt->type == SC_TYPE_ESPTOUCH_V2) {
            // 从中使得esp32的
            ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
            ESP_LOGI(TAG, "RVD_DATA:");
            for (int i=0; i<33; i++) {
                printf("%02x ", rvd_data[i]);
            }
            printf("\n");
        }

        // 断开链接，使用新的配置文件进行配置-并且重连
        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        ESP_LOGI(TAG, "Smartconfig successfully!"); 
        esp_smartconfig_stop();
        all_config.smart_start = false;
        all_config.someone_changed = true;
        smartconfig_successfully = true;
         // /*向start_smartconfig发送配网成功通知，使其解除阻塞状态 */
        xTaskNotifyGive(Smart_task_handle);
    }
}

void initialise_wifi(void)
{
    // 初始化网络接口（esp_netif_init）。
    ESP_ERROR_CHECK(esp_netif_init());
    // 创建一个事件组对象（s_wifi_event_group）来管理WiFi事件的处理。
    s_wifi_event_group = xEventGroupCreate();
    // 创建一个默认事件循环（esp_event_loop_create_default）用于处理系统事件。
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // 创建一个默认的WiFi STA网络接口（esp_netif_create_default_wifi_sta）。
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // #define WIFI_INIT_CONFIG_DEFAULT() { 
    // event_handler 设置 Wi-Fi 事件处理函数；
    //         .event_handler = &wifi_event_handler, 
    // osi_task_pri 和 wifi_task_pri 分别设置了两个 Wi-Fi 任务的优先级；
    //         .osi_task_pri = WIFI_OSi_PRIORITY, 
    //         .wifi_task_pri = WIFI_TASK_PRIORITY, 

    //         .wpa_crypto_funcs = &g_wifi_default_wpa_crypto_funcs, 
    // static_rx_buf_num、dynamic_rx_buf_num、tx_buf_type、static_tx_buf_num、dynamic_tx_buf_num、bcn_buf_num、ampdu_tx_buf_num、ampdu_rx_buf_num 
    // 都是一些缓冲区的数量，用于存放 Wi-Fi 传输中的数据包，配置这些参数可以优化 Wi-Fi 性能；
    //         .static_rx_buf_num = WIFI_STATIC_RX_BUFFER_NUM, 
    //         .dynamic_rx_buf_num = WIFI_DYNAMIC_RX_BUFFER_NUM, 
    //         .tx_buf_type = WIFI_TX_BUFFER_TYPE, 
    //         .static_tx_buf_num = WIFI_STATIC_TX_BUFFER_NUM, 
    //         .dynamic_tx_buf_num = WIFI_DYNAMIC_TX_BUFFER_NUM, 
    //         .bcn_buf_num = WIFI_BCN_BUFFER_NUM, 
    //         .ampdu_tx_buf_num = WIFI_AMPDU_TX_BUFFER_NUM, 
    //         .ampdu_rx_buf_num = WIFI_AMPDU_RX_BUFFER_NUM, 
    //          nvs_enable 则是指示是否启用 NVS 存储来保存 Wi-Fi 配置信息。
    //         .nvs_enable = 1,
    // }
    // 这个宏定义了一系列的 Wi-Fi 初始化配置，例如：
    // nvs_enable 则是指示是否启用 NVS 存储来保存 Wi-Fi 配置信息。
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    
    // 初始化WiFi配置（wifi_init_config_t），并使用它来初始化WiFi（esp_wifi_init）。
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    // 初始化WiFi配置（wifi_init_config_t），并使用它来初始化WiFi（esp_wifi_init）。
    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    // 设置WiFi模式为STA模式（esp_wifi_set_mode）。
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    // 启动WiFi（esp_wifi_start）。
    ESP_ERROR_CHECK( esp_wifi_start() );
    
    // mac地址的获取尝试

    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    ESP_LOGE(TAG, "MAC address: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

//开启配网模式
void start_smartconfig(void)
{
    // 单次配网时长,超过这个时间还没配网则自动重启
    int smartconfig_time = 60; 
    //堵塞等待可写，进入配网模式就获取TCP权限，先暂停其他发送任务
    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
    // 标志位更迭
    smartconfig_successfully = false;
    all_config.smart_start = true;
    all_config.ip_get = false;
    all_config.wifi_connect = false;
    all_config.time_get = false;  
    all_config.someone_changed = true;
    // 配网时数据不要（没意义，且对睡眠分析的算法会造成干扰）
    
    // 开启smartconfig模式
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );

    // 堵塞等待配网，配网成功会触发ESPTOUCH_DONE_BIT事件，最大超时时间为pdMS_TO_TICKS（）ms
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(smartconfig_time * 1000));
    
    // 退出smartconfig模式
    xSemaphoreGive(MuxSem_TCP_Handle);  // 释放TCP限权
    all_config.smart_start = false;
    ESP_LOGI(TAG, "Quit smartconfig mode."); 

}

/// @brief 此处的smart_config仅用于更改nvs中的账号密码，每次开机后自动识别nvs中的账号密码（仅一个）
///         按钮识别后，再次触发smartconfig
///         开机尝试多次第一个账号密码后，触发smartconfig
/// @param parm 
void smartconfig_example_task(void * parm)
{        
    EventBits_t uxBits;

    //此处尝试链接，如果失败再给与信号量
    if(Get_Wifi_NVS()){
        ESP_LOGI(TAG, "Get_Wifi_NVS sucessfully.");
        all_config.ip_get = true;
        all_config.wifi_connect = true;
        all_config.someone_changed = true;
    }else{
        // 获取连接wifi密码失败，自动跳转到配网模式
        ESP_LOGE(TAG, "Get wifi from nvs failed, start to smartconfig.");
        start_smartconfig();
        if(!smartconfig_successfully){  // 若在规定时间没有完成配网操作，则自动重启，防止意外触发的配网
        //堵塞获取各任务句柄限权，等待tcp、txt等任务完成当前操作再重启，避免TCP发送失败、文件数据出错
            // xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
            // xSemaphoreTake(MuxSem_TXT_Handle,portMAX_DELAY);
            // xSemaphoreGive(MuxSem_TXT_Handle);
            ESP_LOGE(TAG, "Smartconfig failed, rebooting esp32.");
            esp_restart();
        }  
    }

    while(1)
    {    
        // 堵塞等待配网按钮事件发生
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | PRESS_KEY_BIT, true, false, portMAX_DELAY);

        if(uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected.");
            all_config.ip_get = true;
            all_config.wifi_connect = true;
            all_config.someone_changed = true;
            xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        }
        if(uxBits & PRESS_KEY_BIT){
            ESP_LOGI(TAG, "Key was pressed, start smartconfig.");
            start_smartconfig();
            if(!smartconfig_successfully){  // 若在规定时间没有完成配网操作，则自动重启，防止意外触发的配网
            //堵塞获取各任务句柄限权，等待tcp、txt等任务完成当前操作再重启，避免TCP发送失败、文件数据出错
                // xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
                // xSemaphoreTake(MuxSem_TXT_Handle,portMAX_DELAY);
                // xSemaphoreGive(MuxSem_TXT_Handle);
                ESP_LOGE(TAG, "Smartconfig failed, rebooting esp32.");
                esp_restart();
            }  
            xEventGroupClearBits(s_wifi_event_group, PRESS_KEY_BIT);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);    
    }

}

// 用于给出一个信号，相当于按配网条件按下了按钮
void smart_config_out_test()
{
    xSemaphoreGive(MuxSem_SMART_Handle);
}