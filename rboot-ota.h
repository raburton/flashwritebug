#ifndef __RBOOT_OTA_H__
#define __RBOOT_OTA_H__

// ota server details
#define OTA_HOST "192.168.7.5"
#define OTA_PORT 80
#define OTA_FILE "/file.bin"

#define SECTOR_SIZE 0x1000
#define FLASH_ADDR 0x100000

// general http header
#define HTTP_HEADER "Connection: keep-alive\r\n\
Cache-Control: no-cache\r\n\
Accept: */*\r\n\r\n"

// timeout for the initial connect and each recv (in ms)
#define OTA_NETWORK_TIMEOUT  10000

// used to indicate a non-rom flash
#define FLASH_BY_ADDR 0xff

// callback method should take this format
typedef void (*flash_callback)(bool result);

// function to perform the ota update
bool ICACHE_FLASH_ATTR flash_start(flash_callback callback);

#endif

