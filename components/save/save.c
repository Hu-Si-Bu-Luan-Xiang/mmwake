#include <stdio.h>
#include "save.h"
#include <stdio.h>
#include <string.h>
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "all_config.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "tcp/include/tcp.h"

#define FILENAME "/extflash/hello.txt"
#define ONCE_DATASIZE ONCE_DATASIZE_CFG
#define MOUNT_PATH "/extflash"
const char *base_path = "/extflash";
static const char *TAG = "FATFS";
static wl_handle_t wl_handle;
// ESP-IDF Wear Levelling 组件的数据类型，均衡磨损 通过wear_levelling_init() 创建一个 Wear Levelling 
// 返回句柄，后续文件中操作这个句柄，添加分区，擦除块

extern SemaphoreHandle_t MuxSem_TXT_Handle;
extern SemaphoreHandle_t MuxSem_TCP_Handle;
extern uint8_t mac[6];
extern int sock;
extern All_Flags all_config;
extern uint8_t id_get[2];
extern const uint8_t Versions[3];                   // 程序版本号
extern u16_t num_busy;                              // 待写入文件的包数
int last_timestamp = 0;                             // 最后入数据库表的时间戳
long start_idx_of_send = 0;                         // 记录数据文件待发送数据的起始下标
int recycle_bin_used_len = 0;                       // 回收站现在缓存的字节数
int boot_time = 0;                                  // 开机时间戳

#ifdef DEBUG
extern u32_t stm_reset_times;                         
extern u32_t stm_radar_failed_times;
extern u32_t stm_data_bags;
extern u32_t disconnection_times;                     
extern u32_t reconnect_times;                         
extern u32_t recv_err_times;                          
extern u32_t num_repeat_time;                         
extern u32_t num_losses_time;                         
extern u32_t num_rectified_repeat;                    
extern u32_t num_rectified_losses;
extern uint8_t num_bag_to_cood;
bool is_first_time_read_file = true;                // 是否第一次读取文件
char str_boot_time[64];                             // 开机时间字符串
long bytes_of_file_reboot    = 0 ;                  // 刚开机时文件已有数据的字节数
long bytes_receive_from_stm = 0 ;                   // 接收到STM发过来的字节数
u32_t send_reconnect_times = 0;                     // 因发送出错关闭sock的次数
u32_t write_file_failed_times = 0;                  // 数据写入文件失败次数
// 记录从开机开始到结束发送历史数据的失败、半失败次数
// 文件里面发送的数据
u32_t send_file_times = 0;                          // TCP发送文件数据的次数
u32_t send_file_success_times = 0;                  // TCP发送文件数据成功的次数
u32_t send_file_err_times = 0;                      // TCP发送文件数据出错的次数
u16_t send_file_part_times = 0;                     // TCP发送文件数据仅部分发送成功的次数
u16_t send_file_return_zero_times = 0;              // TCP发送文件数据返回值为0的次数
u16_t send_file_part_not_complete_times = 0;        // TCP发送文件数据仅部分发送成功且发送数据长度不是BAG_LEN的整数倍的次数
// 回收站里面发送的数据
u32_t send_bin_times = 0;                           // TCP发送回收站数据的次数
u32_t send_bin_success_times = 0;                   // TCP发送回收站数据成功的次数
u32_t send_bin_err_times = 0;                       // TCP发送回收站数据出错的次数
u16_t send_bin_part_times = 0;                      // TCP发送回收站数据仅部分发送成功的次数
u16_t send_bin_return_zero_times = 0;               // TCP发送回收站数据返回值为0的次数
u16_t send_bin_part_not_complete_times = 0;         // TCP发送回收站数据仅部分发送成功且发送数据长度不是BAG_LEN的整数倍的次数
long bytes_write_in_file  = 0 ;                     // 待写入文件的字节数
long bytes_write_in_file_success = 0;               // 写入文件成功数据字节数
long bytes_write_in_file_failed = 0;                // 写入文件失败数据字节数
long bytes_send_to_server_from_file  = 0 ;          // 从文件发送给服务器的字节数
long bytes_send_to_server_from_bin   = 0 ;          // 从回收站发送给服务器的字节数
long bytes_send_to_server_from_file_failed  = 0 ;   // 从文件发送给服务器失败的字节数
long bytes_send_to_server_from_bin_failed   = 0 ;   // 从回收站发送给服务器失败的字节数
long bytes_send_to_server_from_file_success  = 0 ;  // 从文件发送给服务器成功的字节数
long bytes_send_to_server_from_bin_success   = 0 ;  // 从回收站发送给服务器成功的字节数
#endif


static void example_get_fatfs_usage(size_t* out_total_bytes, size_t* out_free_bytes);

/// @brief 测试所余下的空闲和总size
/// @param out_total_bytes 总size
/// @param out_free_bytes 空闲size
static void example_get_fatfs_usage(size_t* out_total_bytes, size_t* out_free_bytes)
{
    // 抽象的fatfs
    FATFS *fs;
    size_t free_clusters;
    // 指的是空闲簇-drv:磁盘编号("0:"/"1:")
    // path：逻辑驱动器的名称，例如 "0:" 表示 SD 卡的根目录
    // nclst：返回参数，指向一个 DWORD 类型的变量，用于返回可用的空闲簇数。
    // fatfs：返回参数，指向一个 FATFS* 类型的指针，用于返回对应的 FATFS 文件系统对象的指针。
    // 空的簇被存在free_clusters中
    int res = f_getfree("0:", (DWORD *)&free_clusters, &fs);
    assert(res == FR_OK);
    ///* Cluster size [sectors] */簇的大小
    // 因为有两个簇用作保存信息
    // 簇是文件系统管理存储空间的基本单位，是文件系统在物理上分配空间的最小单位
    // 在 FAT 文件系统中，簇的大小是一个固定值，通常为 2 的幂次方（例如 2^8、2^9、2^10 等）
    // 当一个文件被存储到磁盘上时，文件系统会根据文件的大小来分配相应的簇数来存储文件。如果文件很小，不足以占据一个完整的簇，那么该簇剩余的空间就浪费掉了
    // 如果文件很大，需要占据多个簇，那么多余的空间也会浪费掉。因此，在设计文件系统时，需要权衡簇的大小，既要保证簇的大小足够存储大文件，也要避免空间浪费。
    // 在 FAT 文件系统中，每个簇都有一个对应的簇号（Cluster Number），文件系统使用簇号来标识存储文件的位置
    // 同时，文件系统会在 FAT 表中记录每个簇的状态，标记该簇是否已被分配或空闲，以便管理磁盘空间
    // ——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
    // fs_type: 文件系统类型，可以是 FS_FAT12、FS_FAT16 或 FS_FAT32。
    // pdrv: 物理驱动器号，用于区分不同的物理存储设备。
    // n_fats: FAT 表的数量，通常为 1 或 2。
    // wflag: 文件系统状态标志，用于表示文件系统是否需要更新。
    // fsi_flag: FATFSINFO 扇区状态标志。
    // id: 文件系统的卷标，通常是 11 个 ASCII 字符。
    // n_rootdir: 根目录区的扇区数量。
    // csize: 簇的扇区数量。
    // n_fatent: FAT 表中的总簇数。
    // fatbase: 第一个 FAT 表的扇区地址。
    // dirbase: 根目录区的起始扇区地址。
    // database: 数据区的起始扇区地址。
    // win: FATFS 内部的临时缓冲区，用于读取和写入簇数据。
    // ——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
    // csize 簇的扇区数量 簇的大小在格式化文件系统时设置，通常是 2、4、8、16、32、64 或 128 个扇区 —— 使用了两个簇用作保存FAT表
    size_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    size_t free_sectors = free_clusters * fs->csize;

    // assuming the total size is < 4GiB, should be true for SPI Flash
    if (out_total_bytes != NULL) {
        *out_total_bytes = total_sectors * fs->ssize;
    }
    if (out_free_bytes != NULL) {
        *out_free_bytes = free_sectors * fs->ssize;
    }
}

/// @brief 挂载
/// @param  
static void initialize_filesystem(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        // 允许打开的最大文件数量
        .max_files = 4,
        .format_if_mount_failed = true
    };
    // MOUNT_PATH：挂载路径 "storage"：文件系统的标识符 &mount_config：挂载选项配置 &wl_handle：一个 wl_handle_t 类型的指针
    // 磨损均衡再wl_handle
    esp_err_t err = esp_vfs_fat_spiflash_mount(MOUNT_PATH, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount file system (%s)", esp_err_to_name(err));
        return;
    }else{
        ESP_LOGI(TAG, "File system mounted");
        // 故意写入随机数并卸载文件系统，搞坏文件文件系统，测试文件系统自动修复程序的有效性
        // FILE* f = fopen(FILENAME, "w");
        // if (f == NULL) {
        //     printf("Failed to open file for writing\n");
        // } else {
        //     // Write random data to the file
        //     char buf[10];
        //     int i;
        //     for (i = 0; i < 10; i++) {
        //         buf[i] = rand() % 256;
        //     }
        //     fwrite(buf, sizeof(buf), 1, f);

        //     // Close the file
        //     fclose(f);
            
        //     // Unmount the filesystem
        //     esp_vfs_fat_spiflash_unmount("/extflash", wl_handle);
        // }
    }
    // 检查文件是否存在
    // FILE* f = fopen("/extflash/hello.txt", "r");
    // if (f == NULL) {  // 文件不存在或者打开失败
    //     ESP_LOGW(TAG, "File not found, formatting partition and creating file...");
    //     // 卸载FAT文件系统
    //     esp_vfs_fat_spiflash_unmount("/extflash", NULL);
    //     // 格式化分区
    //     const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    //     if (partition != NULL) {
    //         esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    //         if (err != ESP_OK)
    //         {
    //             ESP_LOGE(TAG, "Failed to format storage (%s)", esp_err_to_name(err));
    //             return;
    //         }
    //     }
    //     else {
    //         ESP_LOGW(TAG, "Can't found storage partition!");
    //         // 处理未找到分区的情况
    //     }
    //     // 重新挂载FAT文件系统
    //     err = esp_vfs_fat_spiflash_mount("/extflash", "storage", &mount_config, NULL);
    //     if (err != ESP_OK) {  // 挂载失败
    //         ESP_LOGE(TAG, "Failed to mount FATFS after formatting (error %s)", esp_err_to_name(err));
    //         return;
    //     }
    //     // 新建数据文件
    //     f = fopen("/extflash/hello.txt", "w");
    //     if (f == NULL) {  // 文件创建失败
    //         ESP_LOGE(TAG, "Failed to create file /extflash/hello.txt");
    //         return;
    //     } else {
    //         fprintf(f, "Hello World!");  // 写入数据到文件中
    //         fclose(f);
    //         ESP_LOGI(TAG, "File created successfully!");
    //     }
    // } else {  // 文件存在
    //     ESP_LOGI(TAG, "File found, reading contents...");
    //     char buffer[64];
    //     fgets(buffer, sizeof(buffer), f);  // 读取文件内容
    //     fclose(f);
    //     ESP_LOGI(TAG, "Contents of /extflash/hello.txt: %s", buffer);
    // }
}

/// @brief 卸载挂载
/// @param  
void unmount_flash_partition(void) {
    esp_err_t err = esp_vfs_fat_spiflash_unmount(MOUNT_PATH, wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载失败，错误：%s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "卸载成功");
}


/// @brief 清空指定路径文件
/// @param file_name 
void fatfs_clean(const char * file_name)
{
    //此处获取TXT的互斥锁
    xSemaphoreTake(MuxSem_TXT_Handle,portMAX_DELAY);//堵塞等待可写
    // Create a file in FAT FS
    ESP_LOGI(TAG, "cleaning file");
    FILE* f = fopen(file_name, "W");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for cleaning");
        xSemaphoreGive(MuxSem_TXT_Handle);
        return;
    }
    fclose(f);
    ESP_LOGI(TAG, "File cleaned");
    xSemaphoreGive(MuxSem_TXT_Handle);
}

/// @brief fatfs初始化内部flash
/// @param  
void fatfs_init(void)
{
    // 挂载文件系统
    initialize_filesystem();
    // Print FAT FS size information
    size_t bytes_total, bytes_free;
    example_get_fatfs_usage(&bytes_total, &bytes_free);
    ESP_LOGI(TAG, "FAT FS: %d kB total, %d kB free", bytes_total / 1024, bytes_free / 1024);
    if (bytes_free < 1024) {// 数据存满了，则格式化分区内存
        xSemaphoreTake(MuxSem_TXT_Handle,portMAX_DELAY);//堵塞等待可读
        ESP_LOGW(TAG, "bytes_free is not enough, now formatting partition and creating file...");
        // 卸载FAT文件系统
        esp_vfs_fat_spiflash_unmount(MOUNT_PATH, wl_handle);
        // 格式化分区
        const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
        if (partition != NULL) {
            esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to format storage (%s)", esp_err_to_name(err));
            }else{
                ESP_LOGW(TAG, "Format storage successfully! Rebooting...");
            }
        }
        else {
            ESP_LOGW(TAG, "Can't found storage partition!");
            // 处理未找到分区的情况
        }
        xSemaphoreGive(MuxSem_TXT_Handle);
        esp_restart(); // 重启
    }
}

/// @brief 对于等待储存的数据，其实是uint8，且有长度的数据，所有的数据都经过txt，不直接发送
/// @param file_name 
/// @param data 待发送数据
/// @param len 长度
uint8_t fatfs_write(const char * file_name, uint8_t* data, int len)
{
    // ESP_LOGI("RX_TASK_TAG", "--------------------------ftfs协议帧内存--------------------------------");
    // ESP_LOG_BUFFER_HEXDUMP("COOD_DATA", data , BAG_LEN, ESP_LOG_INFO);
    // ESP_LOGI("RX_TASK_TAG", "---------------------------------------------------------------------------");
    
    //此处获取TXT的互斥锁
    if(xSemaphoreTakeFromISR(MuxSem_TXT_Handle, 0) == pdTRUE)
    {
        // ESP_LOGI(TAG, "Opening file");
        // 追加或新建文件
        FILE* f = fopen(file_name, "a+");
        // 检查是否打开成功
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open file for writing");
            ESP_LOGW(TAG, "formatting partition and creating file...");
            // 卸载FAT文件系统
            esp_vfs_fat_spiflash_unmount(MOUNT_PATH, wl_handle);
            // 格式化分区
            const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
            if (partition != NULL) {
                esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to format storage (%s)", esp_err_to_name(err));
                }
            }
            else {
                ESP_LOGW(TAG, "Can't found storage partition!");
                // 处理未找到分区的情况
            }
            xSemaphoreGive(MuxSem_TXT_Handle);
            #ifdef DEBUG
            if (all_config.file_fine != 0) {
                all_config.someone_changed = true;
            }
            #endif
            return 0;
        }
        // ESP_LOGI(TAG, "Will write %d bytes",len);
        // ESP_LOGITAG, "point-befor-write %d \r\n",ftell(f));// 返回当前位置返回值是表示当前位置的长整型值
        int16_t success_len = fwrite(data, sizeof(uint8_t), len, f);
        fseek(f, 0L, SEEK_END);
        long len_of_file = ftell(f);
        ESP_LOGI(TAG, "——————————————————————————  写入数据后文件总长度为： %d  ———————————————————————————", len_of_file);
        size_t bytes_total, bytes_free;
        example_get_fatfs_usage(&bytes_total, &bytes_free);
        ESP_LOGI(TAG, "FAT FS: %d kB total, %d kB free", bytes_total / 1024, bytes_free / 1024);
        // 当空闲内存小于一个扇区大小（4KB）时，清空文件
        if (bytes_free < 4096) {  
            ESP_LOGW(TAG, "File is nearly full! ");
            fclose(f);
            f = fopen(file_name, "w");
            if (f == NULL) {
                ESP_LOGE(TAG, "Failed to open file for cleaning.");
                xSemaphoreGive(MuxSem_TXT_Handle);
                if (all_config.file_fine != 0) {
                    all_config.someone_changed = true;
                }
                return 0;
            }
            start_idx_of_send = 0;
            ESP_LOGI(TAG, "File has been cleaned.");
            fclose(f);
            xSemaphoreGive(MuxSem_TXT_Handle);
            if (all_config.file_fine != 4) {
                all_config.someone_changed = true;
            }
            return 4;
        }
        // ESP_LOGI(TAG, "point-befor %d \r\n",ftell(f)); 

        #ifdef DEBUG
        bytes_write_in_file = bytes_write_in_file + len;
        if (success_len == len) {  
            bytes_write_in_file_success = bytes_write_in_file_success + len;
            if (is_first_time_read_file) {
                is_first_time_read_file = false;
                bytes_of_file_reboot = len_of_file - success_len;
            }
        } else {
            // 文件写入数据失败！
            write_file_failed_times += 1;
            bytes_write_in_file_failed = bytes_write_in_file_failed + len;
            ESP_LOGE(TAG, "Failed to write file, expect %d, but %d.", len, success_len);
        }
        #endif
        // 查看从文件开头1000字节以内的数据
        // 参数 stream 是一个指向打开的文件的指针；参数 offset 是文件偏移量；
        // 参数 whence 是指定偏移量相对位置的基准,只能是 SEEK_SET（文件开头）、SEEK_CUR（文件指针当前位置）、SEEK_END（文件结尾）
        // fseek(f, 0, SEEK_SET);
        // ESP_LOGE(TAG, "point-of-head %d \r\n",ftell(f));
        // uint8_t m_data[ONCE_DATASIZE] = {0};
        // int len_get = (ONCE_DATASIZE > len_of_file ? len_of_file : ONCE_DATASIZE);
        // fread(m_data, sizeof(uint8_t), len_get, f);
        // size_t   fread(   void   *buffer,   size_t   size,   size_t   count,   FILE   *stream   ) 
        // buffer   是读取的数据存放的内存的指针（可以是数组，也可以是新开辟的空间，buffer就是一个索引）   
        // size      是每次读取的字节数  
        // count     是读取次数  
        // stream   是要读取的文件的指针 
        // printf("read n = %ld",read_bytes);
        // ESP_LOGI("RX_TASK_TAG", "--------------------------bin文件内存--------------------------------");
        // //ESP_LOG_BUFFER_HEXDUMP(TAG, m_data,len_get, ESP_LOG_INFO);
        // ESP_LOGI("RX_TASK_TAG", "--------------------------------------------------------------------");

        fclose(f);
        xSemaphoreGive(MuxSem_TXT_Handle);

        #ifdef DEBUG
        if (all_config.file_fine != 1) {
            all_config.someone_changed = true;
        }
        #endif
        return 1;
    }
    #ifdef DEBUG
    if (all_config.file_fine != 2) {
            all_config.someone_changed = true;
    }
    #endif
    return 2;
}

/// @brief 读取文件/回收站缓冲区所有未发送数据并发送
/// @param file_name 数据文件路径
/// @param recycle_bin 回收站暂缓存区
void fatfs_read_all(char * file_name, uint8_t* recycle_bin)
{
    // 堵塞等待可读
    xSemaphoreTake(MuxSem_TXT_Handle, portMAX_DELAY);

    FILE*f = fopen(file_name, "a+");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading and writing.");
        xSemaphoreGive(MuxSem_TXT_Handle);
        return;
    }
    
    bool err_get = false;                                  // 整个发送过程是否出错
    bool is_send_data = false;                             // 是否执行发送数据的步骤
    bool is_clean_file = false;                            // 是否清空文件数据
    bool is_find_start_idx;                                // 是否找到了待发送数据的开始下标
    int i, j, i_time;                                      // i_time:第i个包的时间戳
    int times, fisrt_stamp = 0, last_stamp = 0;            // 发送次数
    int send_len, send_len_of_bin;                         // 每次发送数据的长度
    int rear;                                              // 尾部数据
    long start_idx;                                        // 每次发送数据文件的开始下标
    uint8_t bar[ONCE_DATASIZE] = {0};                      // 待发送数据缓冲区
    // memset(bar, 0, ONCE_DATASIZE);                      // 发送前先将待发送缓冲区清零（不用也行？）

    // 指向文件末端，用于获取文件存储字节数
    fseek(f, 0L, SEEK_END);
    // size_file: 文件数据长度
    long size_file = ftell(f);
    fseek(f, 0L, SEEK_SET);

    #ifdef DEBUG
    uint8_t buf_one_bag[BAG_LEN] = {};                      // 用来读取其中一个数据包的内容
    int file_fisrt_stamp, file_last_stamp;                  // 打印文件首尾数据包的时间戳
    printf("\n");
    ESP_LOGI("ReadFile_TCP","————————————————————————————发——送——状——态—————————————————————————————————————————");
    if ((size_file >= BAG_LEN) && (size_file % BAG_LEN == 0)){
        start_idx = 0;
        fseek(f, start_idx, SEEK_SET);
        fread(buf_one_bag, BAG_LEN, 1, f);
        file_fisrt_stamp = (buf_one_bag[8] << 24) + (buf_one_bag[9] << 16) + (buf_one_bag[10] << 8) + buf_one_bag[11];
        start_idx = size_file - BAG_LEN;
        fseek(f, start_idx, SEEK_SET);
        fread(buf_one_bag, BAG_LEN, 1, f);
        file_last_stamp = (buf_one_bag[8] << 24) + (buf_one_bag[9] << 16) + (buf_one_bag[10] << 8) + buf_one_bag[11];
        ESP_LOGI(TAG, "文件储存字节总数： %ld ; 第一个数据包时间戳：%d ; 最后一个数据包时间戳：%d", size_file, file_fisrt_stamp, file_last_stamp);
    } else {
        ESP_LOGI(TAG, "文件储存字节总数： %ld .", size_file);
    }
    #endif

    // 如果是第一次开机，则有可能之前已经发送一部分但中途失败了，而发过的部分文件又没清理，导致存在数据重复发送的可能，所以需要先检查
    if (all_config.reboot_fisrt_time) {
        // 是开机第一次发送数据
        ESP_LOGW("Reboot"," 开机第一次发送数据...");
        // 数据文件长度没问题
        if ( (size_file > 0) && (size_file % BAG_LEN == 0)) {
            ESP_LOGW("FIND"," File len is ok : %ld .", size_file);
            // 肯定已经收到服务器发过来的最后入表的时间戳了，因为没有收到前是一直堵塞状态的
            // 在tcp.c的rev_cood函数接收到last_timestamp才释放ReadFile_TCP_handle信号量，才第一次执行fatfs_read_all函数
            ESP_LOGW("FIND"," The last_timestamp has been received: %d", last_timestamp);
            is_find_start_idx = false;  // 是否找到了未发送过的数据开始的下标
            rear = size_file % ONCE_DATASIZE;
            times = (size_file / ONCE_DATASIZE) + (rear > 0 ? 1 : 0);
            for(i = 0; i < times; i++) {
                start_idx = i * ONCE_DATASIZE;
                if((i == times-1) && (rear > 0)){
                    // 最后一次
                    send_len = rear;
                }else{
                    // 第 i 次
                    send_len = ONCE_DATASIZE;
                } 
                fseek(f, start_idx, SEEK_SET);
                fread(bar, send_len, 1, f);
                for(j = 0; j < send_len/BAG_LEN; j++){
                    i_time = (bar[8+ j*BAG_LEN] << 24) + (bar[9+ j*BAG_LEN] << 16) + (bar[10+ j*BAG_LEN] << 8) + bar[11+ j*BAG_LEN];
                    if (j == 0){
                        ESP_LOGW("FIND", "process: %d / %d ; fisrt_itime: %d ", i+1, times, i_time);
                    } else if (j == send_len/BAG_LEN - 1) {
                        ESP_LOGW("FIND", "process: %d / %d ; last_itime: %d ", i+1, times, i_time);
                    }
                    if(i_time > last_timestamp && i_time > 0){
                        fisrt_stamp = i_time;
                        start_idx_of_send = start_idx + j * BAG_LEN;  // 找到文件中第一个比最后入表大的时间戳
                        is_find_start_idx = true;
                        ESP_LOGW("FIND","Start_idx_of_send was been found:  %d ; fisrt_stamp: %d ", start_idx_of_send, fisrt_stamp);
                        break;
                    }
                }
                if(is_find_start_idx){
                    break;
                }
            }
            if(!is_find_start_idx){
                // 没有找到未发送过的数据开始的下标（说明文件数据全部都发送过了，可以将文件清掉）
                ESP_LOGW("FIND","All i_time is less than last_timestamp, cleaning file!");  
                is_send_data = false;
                is_clean_file = true;
            } else {
                // 找到了，可以发送数据
                is_send_data = true;
                is_clean_file = false;
            }
        } else {
            is_send_data = false;
            // 数据文件长度不是BAG_LEN的整数倍
            if (size_file % BAG_LEN != 0){
                ESP_LOGE("FIND"," 数据文件长度不是BAG_LEN的整数倍! 清空文件!");
                is_clean_file = true;
            }else{
                is_clean_file = false;
            }
        }
        all_config.reboot_fisrt_time = false;  // 以上程序开机后只会运行一次，所以一定要清除标志位
        all_config.receive_last_timestamp = false;
        all_config.someone_changed = true;
    } else {
        // 不是开机第一次发送数据
        // 如果是重新连接后第一次发送数据
        if (all_config.reconnect_fisrt_time) {
            all_config.reconnect_fisrt_time = false;
            all_config.someone_changed = true;
            ESP_LOGW("Reconnect","重连第一次发送数据...");
            // 回收站缓冲区存有数据
            if ((recycle_bin_used_len >= BAG_LEN) && (recycle_bin_used_len % BAG_LEN == 0)) {
                // 已经收到服务器发过来的最后入表的时间戳
                if (all_config.receive_last_timestamp) { 
                    all_config.receive_last_timestamp = false;
                    #ifdef DEBUG
                    ESP_LOGW("Reconnect"," The last_timestamp has been received: %d", last_timestamp);
                    ESP_LOGW("Recycle Bin:","回收站缓冲数据长度:  %d", recycle_bin_used_len);
                    fisrt_stamp = (recycle_bin[8] << 24) + (recycle_bin[9] << 16) + (recycle_bin[10] << 8) + recycle_bin[11];
                    ESP_LOGW("Recycle Bin:","回收站缓冲最旧的时间戳:  %d", fisrt_stamp);
                    last_stamp = (recycle_bin[recycle_bin_used_len-8] << 24) + (recycle_bin[recycle_bin_used_len-7] << 16) + (recycle_bin[recycle_bin_used_len-6] << 8) + recycle_bin[recycle_bin_used_len-5];
                    ESP_LOGW("Recycle Bin:","回收站缓冲最新的时间戳:  %d", last_stamp);
                    #endif

                    is_find_start_idx = false;  // 是否找到了未发送过的数据开始的下标
                    int start_idx_of_bin = 0, start_idx_resend;
                    fisrt_stamp = 0, last_stamp = 0;
                    for (i = recycle_bin_used_len / BAG_LEN - 1; i >= 0; i--) {
                        i_time = (recycle_bin[8+ i*BAG_LEN] << 24) + (recycle_bin[9+ i*BAG_LEN] << 16) + (recycle_bin[10+ i*BAG_LEN] << 8) + recycle_bin[11+ i*BAG_LEN];
                        if(i_time > last_timestamp){
                            if(last_stamp == 0){
                                last_stamp = i_time;
                            }
                            fisrt_stamp = i_time;
                            start_idx_of_bin = i * BAG_LEN;
                            is_find_start_idx = true;
                        } else {
                            break;
                        }                       
                    }
                    if (is_find_start_idx) {
                        send_len_of_bin = recycle_bin_used_len - start_idx_of_bin;
                        ESP_LOGW("Reconnect","Start_idx_of_bin was been found: %d  ; fisrt_stamp: %d - last_stamp: %d , %d s", start_idx_of_bin, fisrt_stamp, last_stamp, send_len_of_bin/BAG_LEN);
                        rear = send_len_of_bin % ONCE_DATASIZE;
                        times = (send_len_of_bin / ONCE_DATASIZE) + (rear > 0 ? 1 : 0);
                        for (i = 0; i < times; i++){
                            start_idx_resend = i * ONCE_DATASIZE + start_idx_of_bin;
                            if((i == times-1) && (rear > 0)){
                                // 最后一次
                                send_len = rear;
                            }else{
                                // 第 i 次
                                send_len = ONCE_DATASIZE;
                            }
                            // 获取tcp资源
                            xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
                            ESP_LOGW(TAG, "TCP发送回收站数据");
                            // TCP发送 —— 默认是阻塞的 等到发送完成才往下跑
                            int err = send(sock, recycle_bin + start_idx_resend, send_len, 0);
                            #ifdef DEBUG
                            bytes_send_to_server_from_bin += send_len;
                            send_bin_times += 1;
                            #endif
                            xSemaphoreGive(MuxSem_TCP_Handle);
                            ESP_LOGW(TAG, "释放TCP");
                            if (err < 0) {
                                #ifdef DEBUG
                                ESP_LOGE("Reconnect","此次回收站数据发送失败！");
                                bytes_send_to_server_from_bin_failed += send_len;
                                send_bin_err_times += 1;
                                #endif
                                err_get = true;
                                break;
                            } else {
                                #ifdef DEBUG
                                bytes_send_to_server_from_bin_success += err;
                                ESP_LOGW("Reconnect","此次回收站数据发送成功长度:  %d", err);
                                #endif
                                if (err == send_len){
                                    #ifdef DEBUG
                                    send_bin_success_times += 1;
                                    #endif
                                }else{
                                    #ifdef DEBUG
                                    if(err == 0){
                                        send_bin_return_zero_times += 1;
                                    }else{
                                        send_bin_part_times += 1;
                                        if(err%BAG_LEN != 0){
                                            send_bin_part_not_complete_times += 1;
                                        }
                                    }
                                    #endif
                                    err_get = true;
                                    break;
                                }
                            }
                            vTaskDelay(300 / portTICK_PERIOD_MS); 
                        }
                        if(!err_get) {
                            // 全部发送成功
                            ESP_LOGW("Reconnect", "Sent all recycle bin data successfully.");
                            is_clean_file = false;
                            is_send_data = true;
                        }else{
                            // 发送中途出现错误
                            ESP_LOGE("Reconnect", "Sending recycle bin data failed!");
                            is_clean_file = false;
                            is_send_data = false;
                        }

                    } else {
                        ESP_LOGW("Reconnect","All i_time is less than last_timestamp, cleaning bin!");
                        // 缓冲区的时间戳都比最后入表的时间戳小，说明发过了，清空！
                        memset(recycle_bin, 0, RECYCLE_BIN_LEN);
                        recycle_bin_used_len = 0;
                        is_clean_file = false;
                        is_send_data = true;
                    }
                
                } else {
                    // 还没收到服务器发过来的最后入表的时间戳, 则再次向服务器请求
                    ESP_LOGW("Reconnect"," 没有收到服务器发过来的最后入表的时间戳，再次向服务器请求》》》");
                    all_config.reconnect_fisrt_time = true;  // 当没有收到服务器发来的last_timestamp时，再次请求
                    all_config.someone_changed = false;
                    send_mac(0x05, mac);
                    is_send_data = false;
                    is_clean_file = false;
                }

            } else {
                // 缓冲区大小异常，清空！
                memset(recycle_bin, 0, RECYCLE_BIN_LEN);
                recycle_bin_used_len = 0;
                all_config.receive_last_timestamp = false;
                is_clean_file = false;
                is_send_data = true;
            }
            // ESP_LOGW("Reconnect","————————————————————————————————————————————————————————————————————————————————————");
        } else {
            // 不是重连第一次发数据，即正常收发数据
            is_clean_file = false;
            if (size_file > 0){
                ESP_LOGI("SEND"," 正常发送文件数据...");
                is_send_data = true;
            } else {
                is_send_data = false;
            } 
        }
    }

    if (is_send_data) {
        // len_to_be_sent: 总待发送字节数
        long len_to_be_sent = size_file - start_idx_of_send;
        // 一个缓冲区最大是1024，如果总体大于了1024就分次，看分多少次
        rear = len_to_be_sent % ONCE_DATASIZE;
        times = (len_to_be_sent / ONCE_DATASIZE) + (rear > 0 ? 1 : 0);
        
        for (i = 0; i < times; i++)
        {
            // ESP_LOGI("ReadFile_TCP","————————————————————————————发——送——状——态—————————————————————————————————");
            ESP_LOGI("ReadFile_TCP","文件总字节数 : %ld ; 总待发送开始下标 : %ld ; 总待发送字节数 : %ld", size_file, start_idx_of_send, len_to_be_sent);
            // start_idx：此次待发送开始下标，send_len:此次待发送长度
            start_idx = start_idx_of_send + i * ONCE_DATASIZE;
            if (all_config.file_fine == 3 || all_config.ota_start) {  // 待写入文件缓冲区快满了先跳出循环释放TXT限权，或者开启了OTA固件升级模式,先跳出循环释放TCP限权
                if (all_config.file_fine == 3){
                    ESP_LOGW(TAG, "Data_txt_busy buffer to be written is nearly full, jump out of the loop to release TXT limits.");
                } else if (all_config.ota_start) {
                    ESP_LOGW(TAG, "OTA mode is going to start, jump out of the loop to release TCP limits.");
                }

                // 对新的待发送数据文件下标赋值,很重要！！！
                start_idx_of_send = start_idx;
                break;
            }
            if((i == times-1) && (rear > 0)){ // (rear > 0)是一定要加的
                // 最后一次
                send_len = rear;
            }else{
                // 第 i 次
                send_len = ONCE_DATASIZE;
            }
            ESP_LOGI("ReadFile_TCP","发送进度 : %d / %d ; 此次待发送开始下标 : %ld ; 此次待发送字节数 : %ld", i+1, times, start_idx, send_len);
            
            // 将文件指针指向此次待发送开始下标
            fseek(f, start_idx, SEEK_SET);  
            // 将此次待发送内容读取到bar
            fread(bar, 1, send_len, f);
            
            // ESP_LOGI("RX_TASK_TAG", "--------------------------发送的千帧内存--------------------------------");
            // ESP_LOG_BUFFER_HEXDUMP(TAG, bar, ONCE_DATASIZE, ESP_LOG_INFO);
            // ESP_LOGI("RX_TASK_TAG", "-----------------------------------------------------------------------");

            #ifdef DEBUG
            fisrt_stamp = (bar[8] << 24) + (bar[9] << 16) + (bar[10] << 8) + bar[11];
            last_stamp = (bar[send_len-8] << 24) + (bar[send_len-7] << 16) + (bar[send_len-6] << 8) + bar[send_len-5];
            ESP_LOGI("ReadFile_TCP","此次待发送开始时间戳 : %d ; 此次待发送最后时间戳 : %d", fisrt_stamp, last_stamp);
            #endif
            
            // 获取tcp资源
            xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);
            ESP_LOGI(TAG, "TCP发送文件数据");
            // TCP发送 —— 默认是阻塞的 等到发送完成才往下跑
            int err = send(sock, bar, send_len, 0);
            xSemaphoreGive(MuxSem_TCP_Handle);
            ESP_LOGI(TAG, "释放TCP");

            #ifdef DEBUG
            send_file_times += 1;
            bytes_send_to_server_from_file += send_len;
            #endif

            // 假如发送失败，就立刻break重新处理这一帧数据
            if (err <= 0) {
                ESP_LOGE(TAG, "Read_All_1_Error occurred during sending: errno %d", errno);
                #ifdef DEBUG
                bytes_send_to_server_from_file_failed += send_len;
                if (err == 0) {
                    send_file_return_zero_times += 1;
                } else {
                    send_file_err_times += 1;
                }
                #endif     
                // 对新的待发送数据文件下标赋值,很重要！！！
                start_idx_of_send = start_idx;
                err_get = true;
                break;
            } else {
                #ifdef DEBUG
                bytes_send_to_server_from_file_success += err;
                #endif
                // 完全发送成功（成功将待发送数据copy到tcp发送缓冲区）
                if((err == send_len) && (send_len % BAG_LEN == 0)) {
                    #ifdef DEBUG
                    send_file_success_times += 1;
                    #endif
                    // 符合BAG_LEN的整数倍才放入缓冲区，不然可能会出现意想不到的错误
                    ESP_LOGI("SEND","此次数据全部发送成功，发送长度:  %d", send_len);
                    // 将未确认是否真的发送成功的这部分数据先放到recycle_bin先，当网络差断开重连时可回溯，避免丢包问题
                    if (RECYCLE_BIN_LEN - recycle_bin_used_len >= send_len) {
                        memcpy(recycle_bin + recycle_bin_used_len, bar, send_len);
                        recycle_bin_used_len = recycle_bin_used_len + send_len;
                    } else {
                        memmove(recycle_bin, recycle_bin + send_len, RECYCLE_BIN_LEN - send_len);
                        memcpy(recycle_bin + RECYCLE_BIN_LEN - send_len, bar, send_len);
                        recycle_bin_used_len = RECYCLE_BIN_LEN;
                    }
                } else {
                // 部分发送成功（成功将部分待发送数据copy到tcp发送缓冲区，这种情况只会出现在非堵塞模式或者堵塞模式的超时情况下）
                // 部分发送成功或者全部发送成功但发送长度不是BAG_LEN的整数倍也强制关闭sock，重连检查哪些数据已经发送成功了的
                    ESP_LOGW("SEND","此次数据部分发送成功，发送长度:  %d", err);
                    #ifdef DEBUG
                    send_file_part_times += 1;
                    if (err % BAG_LEN != 0) {
                        send_file_part_not_complete_times += 1;
                    } 
                    #endif
                    // 将未确认是否真的发送成功的这部分数据先放到recycle_bin先，断开重连时可回溯验证，避免丢包问题
                    err = BAG_LEN * ( err / BAG_LEN );  // 确保放到回收站的数据是BAG_LEN的整数倍
                    if (RECYCLE_BIN_LEN - recycle_bin_used_len >= err) {
                        memcpy(recycle_bin + recycle_bin_used_len, bar, err);
                        recycle_bin_used_len = recycle_bin_used_len + err;
                    } else {
                        memmove(recycle_bin, recycle_bin + err, RECYCLE_BIN_LEN - err);
                        memcpy(recycle_bin + RECYCLE_BIN_LEN - err, bar, err);
                        recycle_bin_used_len = RECYCLE_BIN_LEN;
                    }
                    start_idx_of_send = start_idx + err;
                    err_get = true;
                    break;
                } 
                #ifdef DEBUG
                ESP_LOGI("Recycle Bin:","回收站缓冲数据长度:  %d", recycle_bin_used_len);
                if((recycle_bin_used_len >= BAG_LEN) && (recycle_bin_used_len % BAG_LEN == 0)){
                    fisrt_stamp = (recycle_bin[8] << 24) + (recycle_bin[9] << 16) + (recycle_bin[10] << 8) + recycle_bin[11];
                    ESP_LOGI("Recycle Bin:","回收站缓冲最旧的时间戳:  %d", fisrt_stamp);
                    last_stamp = (recycle_bin[recycle_bin_used_len-8] << 24) + (recycle_bin[recycle_bin_used_len-7] << 16) + (recycle_bin[recycle_bin_used_len-6] << 8) + recycle_bin[recycle_bin_used_len-5];
                    ESP_LOGI("Recycle Bin:","回收站缓冲最新的时间戳:  %d", last_stamp);
                }
                #endif
            }
            vTaskDelay(300 / portTICK_PERIOD_MS);       
        }
    
        if(!err_get) {
            if (all_config.file_fine == 3) {
                ESP_LOGW(TAG, "Data_txt_busy buffer is nearly full, can't clean file!");
                is_clean_file = false;
            } else if (all_config.ota_start) {
                ESP_LOGW(TAG, "OTA mode is going to start, can't clean file!");
                is_clean_file = false;
            } else {
                ESP_LOGI(TAG, "Sent all data successfully, cleaning file...");
                is_clean_file = true;
            }
        } else {
            ESP_LOGE(TAG, "Sending data failed, can't clean file!");
            is_clean_file = false;
        }
    }

    fclose(f);

    if (is_clean_file) {
        // ESP_LOGI("CleanFile","———————————————————————————————清——理——文——件—————————————————————————————————");
        // 如果发送没出问题，把文件内容全部都发送了，则清空文件    
            f = fopen(file_name, "w");
            if (f == NULL) {
                ESP_LOGE(TAG, "Failed to open file for cleaning.");
                xSemaphoreGive(MuxSem_TXT_Handle);
                return;
            }
            start_idx_of_send = 0;
            ESP_LOGI(TAG, "File has been cleaned.");
            fclose(f);
        // ESP_LOGI("CleanFile","——————————————————————————————————————————————————————————————————————————————");
    }
    
    // 释放文件的权限 全都操作好了，一定不要忘了释放文件信号量限权
    xSemaphoreGive(MuxSem_TXT_Handle);

    if(err_get){
        ESP_LOGE("ReadFile_TCP","————————————————————————————————  发送途中异常  ————————————————————————————————————\r\n");
        // 关掉sock
        if (sock != -1) {         
            ESP_LOGE(TAG, "Shutting down socket...");
            shutdown(sock, 0);
            close(sock);
            sock = -1;
            all_config.tcp_connect = false;
            all_config.someone_changed = true;
            #ifdef DEBUG
            send_reconnect_times += 1;
            #endif
        }
    }else{
        ESP_LOGI("ReadFile_TCP","———————————————————————————————————————————————————————————————————————————————————\r\n");
    }

}

void send_reboot_bag(void)
{
    uint8_t reboot_bag[BAG_LEN] = {0x5a, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0a};
    reboot_bag[2] = id_get[0];
    reboot_bag[3] = id_get[1];
    reboot_bag[5] = Versions[0];
    reboot_bag[6] = Versions[1];
    reboot_bag[7] = Versions[2];
    reboot_bag[8] = (uint8_t)(boot_time>>24);
    reboot_bag[9] = (uint8_t)(boot_time>>16);
    reboot_bag[10] = (uint8_t)(boot_time>>8);
    reboot_bag[11] = (uint8_t)boot_time;
    xSemaphoreTake(MuxSem_TCP_Handle, portMAX_DELAY);  // 堵塞等待TCP空闲
    ESP_LOGW(TAG, "获取TCP -- 发送ESP32重启数据包");
    int err = send(sock, reboot_bag, BAG_LEN, 0);
    xSemaphoreGive(MuxSem_TCP_Handle);
    ESP_LOGW(TAG, "释放TCP");
    if (err == BAG_LEN) {
        ESP_LOGW(TAG, "发送成功.");
    } else {
        ESP_LOGE(TAG, "发送失败: errno %d", errno);
    }
}

//read_all and tcp send when timer clock
void ReadFile_TCP(void *arg)
{
    ESP_LOGI("ReadFile_TCP","任务创建成功");
    char file_1[] = TXT_PATH;
    uint8_t recycle_bin[RECYCLE_BIN_LEN] = {0};
    // 堵塞，等待开机后获取到 最后入表时间戳和设备id再执行
    ulTaskNotifyTake( pdTRUE, portMAX_DELAY);
    send_reboot_bag();
    while(1)
    {
        //每分钟唤醒一次
        vTaskDelay((TIME_OF_SEND * 1000) / portTICK_RATE_MS);
        /// @brief 等待timer资源
        /// @param arg      
        if(all_config.ip_get && all_config.tcp_connect && sock != -1 && !all_config.ota_start)
        {
            //目标缓冲区
            fatfs_read_all(file_1, recycle_bin);
        }
        #ifdef DEBUG
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        char str[64];
        strftime(str, sizeof(str), "%c", &timeinfo);
        ESP_LOGI("State", "—————————————————————————————————————————————设——备——%d——运——行——状——态—————————————————————————————————————————————————", id_get[0]*256 + id_get[1]);
        ESP_LOGI("State", " 开 机 时 间：%s , boot_time: %d ;", str_boot_time, boot_time);
        ESP_LOGI("State", " 当 前 时 间：%s , now_time : %d ;", str, now);
        ESP_LOGI("State", " 累 计 运 行：%d s  ( 约 %.3f h )", now-boot_time, (now-boot_time)/3600.0f);
        ESP_LOGI("State", " 开机文件已有数据 :%8ld  s      ;%8ld  bytes ", bytes_of_file_reboot/BAG_LEN, bytes_of_file_reboot);
        ESP_LOGI("State", " 接收的单片机数据 :%8d  s      ;%8ld  bytes (data: %d ; reset: %d ; fail: %d)", stm_data_bags+stm_radar_failed_times+stm_reset_times, bytes_receive_from_stm, stm_data_bags, stm_reset_times, stm_radar_failed_times);
        ESP_LOGI("State", " 待写入文件缓存数 :%8d  s      ;%8d  bytes", num_busy+num_bag_to_cood, (num_bag_to_cood+num_busy)*BAG_LEN);
        ESP_LOGI("State", " 写入文件数据：sum:%8ld; success:%8ld; failed:%8ld s (sum:%8ld; success:%8ld; failed:%8ld bytes)", bytes_write_in_file/BAG_LEN, bytes_write_in_file_success/BAG_LEN, bytes_write_in_file_failed/BAG_LEN,
                             bytes_write_in_file, bytes_write_in_file_success, bytes_write_in_file_failed);
        ESP_LOGI("State", " 文件发送数据：sum:%8ld; success:%8ld; failed:%8ld s (sum:%8ld; success:%8ld; failed:%8ld bytes)", 
                             bytes_send_to_server_from_file/BAG_LEN, bytes_send_to_server_from_file_success/BAG_LEN, bytes_send_to_server_from_file_failed/BAG_LEN,
                             bytes_send_to_server_from_file, bytes_send_to_server_from_file_success, bytes_send_to_server_from_file_failed );
        ESP_LOGI("State", " 缓存发送数据：sum:%8ld; success:%8ld; failed:%8ld s (sum:%8ld; success:%8ld; failed:%8ld bytes)", 
                             bytes_send_to_server_from_bin/BAG_LEN, bytes_send_to_server_from_bin_success/BAG_LEN, bytes_send_to_server_from_bin_failed/BAG_LEN,
                             bytes_send_to_server_from_bin, bytes_send_to_server_from_bin_success, bytes_send_to_server_from_bin_failed );
        ESP_LOGI("State", " 文件发送次数：sum:%8d; success:%8d; failed:%8d ; part:%8d; zero:%8d; not_complete:%8d 次", send_file_times, send_file_success_times, send_file_err_times ,
                             send_file_part_times, send_file_return_zero_times, send_file_part_not_complete_times);
        ESP_LOGI("State", " 缓存发送次数：sum:%8d; success:%8d; failed:%8d ; part:%8d; zero:%8d; not_complete:%8d 次", send_bin_times, send_bin_success_times, send_bin_err_times ,
                             send_bin_part_times, send_bin_return_zero_times, send_bin_part_not_complete_times);
        ESP_LOGI("State", " Socket recv断开次数: %d ; send断开次数: %d ; 接收监听出错次数: %d .", reconnect_times, send_reconnect_times, recv_err_times);
        ESP_LOGI("State", "—————————————————————————————————————————————————Versions %d.%d.%d————————————————————————————————————————————————————————\r\n", Versions[0], Versions[1], Versions[2]);
        #endif
    }
}


//——————————————————————————————————————————————————————————测试区域————————————————————————————————————————————————————————————————————————
// char line[ONCE_DATASIZE];
// uint8_t line_my[ONCE_DATASIZE + 1];
// char *pos;

/// @brief 驱动对外的测试接口，作为一次性运行
/// @param  
// void test_fatfs_out(void)
// {
//     // test_write_read();
// }

/// @brief 测试堵塞状态下，多次将准备释放的data转化成字符串存放，等到积攒n个后，开始取出
/// @param  
// void test_write_read(void)
// {
//     fatfs_init();
//     char file_1[] = "/extflash/hello.txt";
//     fatfs_clean(file_1);   
//     //制作data，长度定
//     uint8_t data[1024];
//     int len = 1024;
//     for (int i = 0; i < len; i++)
//     {
//         data[i] = i;
//     }
//     uint8_t read_data[1024];
//     int data_len = 1024;
//     for (int i = 0; i < data_len; i++)
//     {
//         read_data[i] = i;
//     }
//     vTaskDelay(50);
//     //模拟堵塞的时候写入
//     fatfs_write(file_1,data,len);
//     fatfs_write(file_1,data,len);
//     fatfs_write(file_1,data,len);
//     fatfs_write(file_1,data,len);
//     vTaskDelay(50);
//     fatfs_read_all(file_1,line_my,ONCE_DATASIZE);
//     //测试成功
// }


/// @brief 仅仅用作检测读写
/// @param  
// void test_fatfs(void)
// {
//     initialize_filesystem();
//     // Print FAT FS size information
//     size_t bytes_total, bytes_free;
//     example_get_fatfs_usage(&bytes_total, &bytes_free);
//     ESP_LOGI(TAG, "FAT FS: %d kB total, %d kB free", bytes_total / 1024, bytes_free / 1024);
//     // Create a file in FAT FS
//     ESP_LOGI(TAG, "Opening file");
//     FILE* f = fopen("/extflash/hello.txt", "a");
//     if (f == NULL) {
//         ESP_LOGE(TAG, "Failed to open file for writing");
//         return;
//     }
//     //使用fprintf写入字符串 + esp32的版本号
//     fprintf(f, "Written u将11148645646cghchvyj894648961111红2gIDF %s\n", esp_get_idf_version());
//     fclose(f);
//     ESP_LOGI(TAG, "File written");
//     // Open file for reading
//     ESP_LOGI(TAG, "Reading file");
//     f = fopen("/extflash/hello.txt", "r");
//     if (f == NULL) {
//         ESP_LOGE(TAG, "Failed to open file for reading");
//         return;
//     }
//     memset(&line,0,sizeof(line));
//     //fgets获取128个字符，从文件中
//     fgets(line, sizeof(line), f);
//     ESP_LOG_BUFFER_HEXDUMP(TAG, line, sizeof(line), ESP_LOG_INFO);
//     printf("%ld \r\n",ftell(f));
//     memset(&line,0,sizeof(line));
//     fgets(line, sizeof(line), f);
//     ESP_LOG_BUFFER_HEXDUMP(TAG, line, sizeof(line), ESP_LOG_INFO);
//     printf("%ld \r\n",ftell(f));
//     fclose(f);
//     //找到'\n'第一次出现的地方
//     pos = strchr(line, '\n');
//     if (pos) {
//         *pos = '\0';
//     }
//     ESP_LOGI(TAG, "Read from file: '%s'", line);
//     //卸载文件系统，此函数在下文定义
//     unmount_flash_partition();
// }