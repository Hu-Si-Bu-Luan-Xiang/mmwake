#ifndef SAVE
#define SAVE

void ReadFile_TCP(void *arg);
uint8_t fatfs_write(const char * file_name, uint8_t* data, int len);
void fatfs_clean(const char * file_name);
void fatfs_read_all(char * file_name, uint8_t *recycle_bin);
void fatfs_init(void);
void keep_connect(void *pvParameters);
void stay_for_check_wifi_for_server(void *pvParameters);
void send_reboot_bag(void);

#endif