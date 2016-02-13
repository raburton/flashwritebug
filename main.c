//////////////////////////////////////////////////
// rBoot sample project.
// Copyright 2015 Richard A Burton
// richardaburton@gmail.com
// See license.txt for license terms.
//////////////////////////////////////////////////

#include <c_types.h>
#include <osapi.h>
#include <user_interface.h>
#include <time.h>
#include <mem.h>

#include "main.h"
#include "user_config.h"
#include "rboot-ota.h"
#include "uart.h"

static os_timer_t network_timer;

void ICACHE_FLASH_ATTR user_rf_pre_init() {
}

void ICACHE_FLASH_ATTR network_wait_for_ip() {

	struct ip_info ipconfig;
	os_timer_disarm(&network_timer);
	wifi_get_ip_info(STATION_IF, &ipconfig);
	if (wifi_station_get_connect_status() == STATION_GOT_IP && ipconfig.ip.addr != 0) {
		char page_buffer[40];
		os_sprintf(page_buffer,"ip: %d.%d.%d.%d\r\n",IP2STR(&ipconfig.ip));
		uart0_send(page_buffer);
	} else {
		char page_buffer[40];
		os_sprintf(page_buffer,"network retry, status: %d\r\n",wifi_station_get_connect_status());
		if(wifi_station_get_connect_status() == 3) wifi_station_connect();
		uart0_send(page_buffer);
		os_timer_setfn(&network_timer, (os_timer_func_t *)network_wait_for_ip, NULL);
		os_timer_arm(&network_timer, 2000, 0);
	}
}

void ICACHE_FLASH_ATTR wifi_config_station() {

	struct station_config stationConf;

	wifi_set_opmode(0x1);
	stationConf.bssid_set = 0;
	os_strcpy(&stationConf.ssid, WIFI_SSID, os_strlen(WIFI_SSID));
	os_strcpy(&stationConf.password, WIFI_PWD, os_strlen(WIFI_PWD));
	wifi_station_set_config(&stationConf);
	uart0_send("wifi connecting...\r\n");
	wifi_station_connect();
	os_timer_disarm(&network_timer);
	os_timer_setfn(&network_timer, (os_timer_func_t *)network_wait_for_ip, NULL);
	os_timer_arm(&network_timer, 2000, 0);
}

void ICACHE_FLASH_ATTR show_ip() {
	struct ip_info ipconfig;
	char msg[50];
	wifi_get_ip_info(STATION_IF, &ipconfig);
	if (wifi_station_get_connect_status() == STATION_GOT_IP && ipconfig.ip.addr != 0) {
		os_sprintf(msg, "ip: %d.%d.%d.%d, mask: %d.%d.%d.%d, gw: %d.%d.%d.%d\r\n",
			IP2STR(&ipconfig.ip), IP2STR(&ipconfig.netmask), IP2STR(&ipconfig.gw));
	} else {
		os_sprintf(msg, "network status: %d\r\n", wifi_station_get_connect_status());
	}
	uart0_send(msg);
}

static void ICACHE_FLASH_ATTR Flash_CallBack(bool result) {
	if(result == true) {
		// success
		// set to boot new rom and then reboot
		uart0_send("File flashed, restarting to ensure clean rom cache...\r\n");
		system_restart();
	} else {
		// fail
		uart0_send("Flash failed!\r\n");
	}
}


uint32 ICACHE_FLASH_ATTR crc32b(uint32 crc, uint8 *data, int32 len) {
   int32 i, j;
   uint32 byte, mask;

   for (i = 0; i < len; i++) {
      byte = data[i];            // Get next byte.
      crc = crc ^ byte;
      for (j = 7; j >= 0; j--) {    // Do eight times.
         mask = -(crc & 1);
         crc = (crc >> 1) ^ (0xEDB88320 & mask);
      }
   }
   return crc;
}

void ICACHE_FLASH_ATTR Checksum(void) {

	int i;
	uint8 data[1024];
	uint32 crc = 0xffffffff;

	for (i = 0; i < 256; i++) {
		spi_flash_read(FLASH_ADDR + (1024 * i), (uint32*)data, 1024);
		crc = crc32b(crc, data, 1024);
	}
	crc = ~crc;
	os_sprintf((char*)data, "crc of 0x%x to 0x%x = 0x%x\r\n", FLASH_ADDR, (FLASH_ADDR + (1024 * i)) - 1, crc);
	uart0_send((char*)data);
}

void ICACHE_FLASH_ATTR ProcessCommand(char* str) {
	if (!strcmp(str, "help")) {
		uart0_send("available commands\r\n");
		uart0_send("  help - display this message\r\n");
		uart0_send("  ip - show current ip address\r\n");
		uart0_send("  connect - connect to wifi\r\n");
		uart0_send("  restart - restart the esp8266\r\n");
		uart0_send("  flash - perform a flash and reboot\r\n");
		uart0_send("  crc - calculate crc for flash area\r\n");
		uart0_send("\r\n");
	} else if (!strcmp(str, "connect")) {
		wifi_config_station();
	} else if (!strcmp(str, "crc")) {
		Checksum();
	} else if (!strcmp(str, "restart")) {
		uart0_send("Restarting...\r\n\r\n");
		system_restart();
	} else if (!strcmp(str, "flash")) {
		// start the flash process
		if (flash_start((flash_callback)Flash_CallBack)) {
			uart0_send("Downloading and flashing...\r\n");
		} else {
			uart0_send("Flash start failed!\r\n\r\n");
		}
	} else if (!strcmp(str, "ip")) {
		show_ip();
	}
}

void ICACHE_FLASH_ATTR user_init(void) {

	char msg[50];

	uart_init(BIT_RATE_115200,BIT_RATE_115200);
	uart0_send("\r\n\r\nsdk v1.5.1+ flash bug demo\r\n");
	uart0_send(msg);

	uart0_send("type \"help\" and press <enter> for help...\r\n");

}
