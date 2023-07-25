#ifndef ALL_CONFIG
#define ALL_CONFIG
#define DEBUG               // 需不需要打印log信息
#define LED_NEW_BOARD       // 是否新的底板

#define ID 0

#define TX_PIN GPIO_NUM_19
#define RX_PIN GPIO_NUM_18

// 串口API的BUF长度
#define Serial_TX_BUF_LEN 2048
#define Serial_RX_BUF_LEN 2048

// -------------------------------------------FLASH存储区域-------------------------------------------------------
// FLASH存储文档PATH
#define TXT_PATH "/extflash/hello.txt"
#define ONCE_DATASIZE_CFG 1024
// -------------------------------------------串口包格式区域-------------------------------------------------------

#define serial_len 8
// -------------------------------------------数据包格式区域-------------------------------------------------------

#define HEAD_SEND1 0x5a


#define END_SEND1 0x0d
#define END_SEND2 0x0a

#define BAG_LEN 16

// ---------------------------------------------------------------------------------------------------------------

// -------------------------------------------TCP连接区域-------------------------------------------------------

// TCP包的接收长度
#define BAG_REC_LEN 16

// 心跳包长度——内容
#define LEN_OF_HEART 1
#define TXT_OF_HEART 0x01

#define TIME_OF_RECONNECT 30  // 断开重连的时间
#define TIME_OF_SEND 60       // 发送数据包的间隔时间
#define TIMEOT_OF_SEND 30     // 单次发送超时时间
#define TIME_OF_CRY 5         // 发送心跳包的间隔时间
#define TIME_OF_WRITE 64      // 写入文件的最小数据秒数
// 接受缓冲区的长度
#define LEN_OF_REC 128
#define CODING_BUF_LEN  BAG_LEN*TIME_OF_WRITE*4  // 待写入文件数据缓冲区最大长度

#define RECYCLE_BIN_LEN 4096*4  // 回收站缓冲区长度

// OTA升级状态码
#define OTA_START    0x01  // 开始升级
#define OTA_LASTED   0x02  // 无需升级
#define OTA_FAILED   0x04  // 升级失败
#define OTA_SUCCESS  0x14  // 升级成功


// 调试服务器目标宏，按照所需进行选择
#define TEST2

#ifdef TEST1
#define IPV4_ADDR "192.168.1.109"
#define PORT_MY 8888
#endif

#ifdef TEST2
#define IPV4_ADDR "8.134.111.197"
#define PORT_MY 9099
#endif

#ifdef TEST3
#define IPV4_ADDR "43.139.32.192"
#define PORT_MY 8000
#endif

// -------------------------------------------按钮区域-------------------------------------------------------
// 定义长按的时间
#define LONG_PRES_TIME_S 1
// KEY活跃电平定义
#define BUTTON_IO_NUM_MY  0
#define BUTTON_ACTIVE_LEVEL_MY 0

// -------------------------------------------LED区域-------------------------------------------------------
#ifdef LED_NEW_BOARD
#define LED_IO_ORANGE 4
#define LED_IO_BLUE   5
#define LED_IO_RED    6
#define LED_IO_GREEN  7
#else
#define LED_IO_1   3
#define LED_IO_2   8
#endif
// LED活跃状态的电平
#define LED_TURN_OFF_MY    1
#define LED_TURN_ON_MY     0

// ESP32接收到STM32上传字符解析协议
#define MODE_A  (uint8_t)('a')    // 若接收到'a'，代表STM32正在运行APP程序
#define MODE_B  (uint8_t)('b')    // 若接收到'b'，代表STM32正在运行BOOTLOADER程序，等待下载固件包或者跳转到APP运行的指令
#define MODE_C  (uint8_t)('c')    // 若接收到'c'，代表已经完成固件的下载过程了（可能成功，可能失败）
#define MODE_D  (uint8_t)('d')    // 若接收到'd'，代表固件正在下载
#define IAP_SUCCESS  (uint8_t)('s')  // 若接收到's'，代表固件下载成功
#define IAP_FAILED   (uint8_t)('f')  // 若接收到'f'，代表固件下载失败

// ESP32发送给ESP32字符定义
#define CHECK_MODE   (uint8_t)('m')  // 发送'm'代表ESP32想知道STM32现在运行在哪个模式（循环）里
#define UPGRADE      (uint8_t)('u')  // 发送'u'代表ESP32想让STM32下载固件包
#define REBOOT       (uint8_t)('b')  // 发送'b'代表ESP32想让STM32重启进入Bootloader程序
#define RUN_APP      (uint8_t)('r')  // 发送'r'代表ESP32想让STM32跳转至APP程序运行
#define CHECK_RESULT (uint8_t)('c')  // 发送'c'代表ESP32想知道STM32端固件是否下载成功
#define CANCEL       (uint8_t)(0x18) // 发送0x18代表ESP32想让STM32退出下载模式

typedef enum {
    STM_DISCONNECT = 0,           // STM32失联
    STM_RUNNING_BOOTLOADER,       // 进入bootloader程序
    STM_RUNNING_APP,              // 正在运行APP
    STM_DOWNLOADING,              // 正在IAP下载
    STM_DOWNLOADED,               // IAP下载完成
    STM_IAP_FAILED,               // IAP失败
    STM_IAP_SUCCESS,              // IAP成功
} stm_state_t;                    // 这个枚举的作用是让ESP32的程序处理逻辑更清晰

//全部标志位
typedef struct ALL_FLAG
{
    bool wifi_connect;
    bool ip_get;                  // 是否被分配到ip地址（连接上网络）
    bool tcp_connect;             // 是否连接上TCP服务
    bool time_get;                // 是否已经获取到世界时间
    bool can_cood;                // 有数据接收到，可以进行时间打点处理
    bool smart_start;             // 是否是在smart config模式
    bool ota_start;               // 是否是在OTA模式（ESP32远程固件升级）
    bool iap_start;               // 是否是在IAP模式（STM32应用内编程）
    bool id_get;                  // 是否获取到设备id标志位
    bool reboot_fisrt_time;       // 是否重启后第一次发送数据
    bool reconnect_fisrt_time;    // 是否重连socket后第一次发送数据
    bool receive_last_timestamp;  // 是否获取到最后一次入表时间
    bool someone_changed;         // 是否有某些标志位改变了，有就打印显示出来
    bool stm_radar_failed;        // 用于判断雷达通讯或者STM32是否正常
    stm_state_t stm32_state;      // 用于判断STM32处于哪种运行模式
    uint8_t rev_bag;              // 用于判断STM32单片机是否正常工作
    uint8_t file_fine;            // 文件状态，取值 {0：文件不存在或已损坏  1：正常  2：暂时获取不到文件限权  3：待写入文件的缓冲区快满了，请求文件限权  4：文件空间内存已不足，清空文件} 
    
}All_Flags;

#endif