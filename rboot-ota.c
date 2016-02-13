#include <c_types.h>
#include <user_interface.h>
#include <espconn.h>
#include <mem.h>
#include <osapi.h>

#include "rboot-ota.h"

typedef struct {
	uint32 start_addr;
	uint32 start_sector;
	uint32 last_sector_erased;
	uint8 extra_count;
	uint8 extra_bytes[4];
} flash_write_status;

typedef struct {
	flash_callback callback;
	uint32 total_len;
	uint32 content_len;
	struct espconn *conn;
	bool success;
	ip_addr_t ip;
	flash_write_status write_status;
} upgrade_status;

static upgrade_status *upgrade;
static os_timer_t ota_timer;

// create the write status struct, based on supplied start address
flash_write_status ICACHE_FLASH_ATTR flash_write_init(uint32 start_addr) {
	flash_write_status status = {0};
	status.start_addr = start_addr;
	status.start_sector = start_addr / SECTOR_SIZE;
	return status;
}

// function to do the actual writing to flash
// call repeatedly with more data (max len per write is the flash sector size (4k))
bool ICACHE_FLASH_ATTR flash_write_flash(flash_write_status *status, uint8 *data, uint16 len) {
	
	bool ret = false;
	uint8 *buffer;
	
	if (data == NULL || len == 0) {
		return true;
	}
	
	// get a buffer
	buffer = (uint8 *)os_malloc(len + status->extra_count);
	if (!buffer) {
		//os_printf("No ram!\r\n");
		return false;
	}

	// copy in any remaining bytes from last chunk
	memcpy(buffer, status->extra_bytes, status->extra_count);
	// copy in new data
	memcpy(buffer + status->extra_count, data, len);

	// calculate length, must be multiple of 4
	// save any remaining bytes for next go
	len += status->extra_count;
	status->extra_count = len % 4;
	len -= status->extra_count;
	memcpy(status->extra_bytes, buffer + len, status->extra_count);

	if (len > SECTOR_SIZE) {
		// to support larger writes we would need to erase current
		// (if not already done), next and possibly later sectors too
	} else {
		// check if the sector the write finishes in has been erased yet,
		// this is fine as long as data len < sector size
		if (status->last_sector_erased != (status->start_addr + len) / SECTOR_SIZE) {
			status->last_sector_erased = (status->start_addr + len) / SECTOR_SIZE;
			spi_flash_erase_sector(status->last_sector_erased);
		}
	}

	// write current chunk
	os_printf("write addr: 0x%08x, len: 0x%04x\r\n", status->start_addr, len);
	if (spi_flash_write(status->start_addr, (uint32 *)buffer, len) == SPI_FLASH_RESULT_OK) {
		ret = true;
		status->start_addr += len;
	}

	os_free(buffer);
	return ret;
}

// clean up at the end of the update
// will call the user call back to indicate completion
void ICACHE_FLASH_ATTR flash_deinit() {

	bool result;
	flash_callback callback;
	struct espconn *conn;

	os_timer_disarm(&ota_timer);

	// save only remaining bits of interest from upgrade struct
	// then we can clean it up early, so disconnect callback
	// can distinguish between us calling it after update finished
	// or being called earlier in the update process
	conn = upgrade->conn;
	callback = upgrade->callback;
	result = upgrade->success;

	// clean up
	os_free(upgrade);
	upgrade = 0;

	// if connected, disconnect and clean up connection
	if (conn) espconn_disconnect(conn);

	// call user call back
	if (callback) {
		callback(result);
	}

}

// called when connection receives data (hopefully the rom)
static void ICACHE_FLASH_ATTR upgrade_recvcb(void *arg, char *pusrdata, unsigned short length) {

	char *ptrData, *ptrLen, *ptr;

	// disarm the timer
	os_timer_disarm(&ota_timer);

	// first reply?
	if (upgrade->content_len == 0) {
		// valid http response?
		if ((ptrLen = (char*)os_strstr(pusrdata, "Content-Length: "))
			&& (ptrData = (char*)os_strstr(ptrLen, "\r\n\r\n"))
			&& (os_strncmp(pusrdata + 9, "200", 3) == 0)) {

			// end of header/start of data
			ptrData += 4;
			// length of data after header in this chunk
			length -= (ptrData - pusrdata);
			// running total of download length
			upgrade->total_len += length;
			// process current chunk
			if (!flash_write_flash(&upgrade->write_status, (uint8*)ptrData, length)) {
				// write error
				flash_deinit();
				return;
			}
			// work out total download size
			ptrLen += 16;
			ptr = (char *)os_strstr(ptrLen, "\r\n");
			*ptr = '\0'; // destructive
			upgrade->content_len = atoi(ptrLen);
		} else {
			// fail, not a valid http header/non-200 response/etc.
			flash_deinit();
			return;
		}
	} else {
		// not the first chunk, process it
		upgrade->total_len += length;
		if (!flash_write_flash(&upgrade->write_status, (uint8*)pusrdata, length)) {
			// write error
			flash_deinit();
			return;
		}
	}

	// check if we are finished
	if (upgrade->total_len == upgrade->content_len) {
		upgrade->success = true;
		// clean up and call user callback
		flash_deinit();
	} else if (upgrade->conn->state != ESPCONN_READ) {
		// fail, but how do we get here? premature end of stream?
		flash_deinit();
	} else {
		// timer for next recv
		os_timer_setfn(&ota_timer, (os_timer_func_t *)flash_deinit, 0);
		os_timer_arm(&ota_timer, OTA_NETWORK_TIMEOUT, 0);
	}
}

// disconnect callback, clean up the connection
// we also call this ourselves
static void ICACHE_FLASH_ATTR upgrade_disconcb(void *arg) {
	// use passed ptr, as upgrade struct may have gone by now
	struct espconn *conn = (struct espconn*)arg;

	os_timer_disarm(&ota_timer);
	if (conn) {
		if (conn->proto.tcp) {
			os_free(conn->proto.tcp);
		}
		os_free(conn);
	}

	// is upgrade struct still around?
	// if so disconnect was from remote end, or we called
	// ourselves to cleanup a failed connection attempt
	// must ensure disconnect was for this upgrade attempt,
	// not a previous one! this call back is async so another
	// upgrade struct may have been created already
	if (upgrade && (upgrade->conn == conn)) {
		// mark connection as gone
		upgrade->conn = 0;
		// end the update process
		flash_deinit();
	}
}

// successfully connected to update server, send the request
static void ICACHE_FLASH_ATTR upgrade_connect_cb(void *arg) {

	uint8 *request;

	// disable the timeout
	os_timer_disarm(&ota_timer);

	// register connection callbacks
	espconn_regist_disconcb(upgrade->conn, upgrade_disconcb);
	espconn_regist_recvcb(upgrade->conn, upgrade_recvcb);

	// http request string
	request = (uint8 *)os_malloc(512);
	if (!request) {
		uart0_send("No ram!\r\n");
		flash_deinit();
		return;
	}
	os_sprintf((char*)request,
		"GET " OTA_FILE " HTTP/1.1\r\nHost: " OTA_HOST "\r\n" HTTP_HEADER);

	// send the http request, with timeout for reply
	os_timer_setfn(&ota_timer, (os_timer_func_t *)flash_deinit, 0);
	os_timer_arm(&ota_timer, OTA_NETWORK_TIMEOUT, 0);
	espconn_sent(upgrade->conn, request, os_strlen((char*)request));
	os_free(request);
}

// connection attempt timed out
static void ICACHE_FLASH_ATTR connect_timeout_cb() {
	uart0_send("Connect timeout.\r\n");
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	upgrade_disconcb(upgrade->conn);
}

static const char* ICACHE_FLASH_ATTR esp_errstr(sint8 err) {
	switch(err) {
		case ESPCONN_OK:
			return "No error, everything OK.";
		case ESPCONN_MEM:
			return "Out of memory error.";
		case ESPCONN_TIMEOUT:
			return "Timeout.";
		case ESPCONN_RTE:
			return "Routing problem.";
		case ESPCONN_INPROGRESS:
			return "Operation in progress.";
		case ESPCONN_ABRT:
			return "Connection aborted.";
		case ESPCONN_RST:
			return "Connection reset.";
		case ESPCONN_CLSD:
			return "Connection closed.";
		case ESPCONN_CONN:
			return "Not connected.";
		case ESPCONN_ARG:
			return "Illegal argument.";
		case ESPCONN_ISCONN:
			return "Already connected.";
	}
}

// call back for lost connection
static void ICACHE_FLASH_ATTR upgrade_recon_cb(void *arg, sint8 errType) {
	uart0_send("Connection error: ");
	uart0_send(esp_errstr(errType));
	uart0_send("\r\n");
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	upgrade_disconcb(upgrade->conn);
}

// call back for dns lookup
static void ICACHE_FLASH_ATTR upgrade_resolved(const char *name, ip_addr_t *ip, void *arg) {

	if (ip == 0) {
		uart0_send("DNS lookup failed for: ");
		uart0_send(OTA_HOST);
		uart0_send("\r\n");
		// not connected so don't call disconnect on the connection
		// but call our own disconnect callback to do the cleanup
		upgrade_disconcb(upgrade->conn);
		return;
	}

	// set up connection
	upgrade->conn->type = ESPCONN_TCP;
	upgrade->conn->state = ESPCONN_NONE;
	upgrade->conn->proto.tcp->local_port = espconn_port();
	upgrade->conn->proto.tcp->remote_port = OTA_PORT;
	*(ip_addr_t*)upgrade->conn->proto.tcp->remote_ip = *ip;
	// set connection call backs
	espconn_regist_connectcb(upgrade->conn, upgrade_connect_cb);
	espconn_regist_reconcb(upgrade->conn, upgrade_recon_cb);

	// try to connect
	espconn_connect(upgrade->conn);

	// set connection timeout timer
	os_timer_disarm(&ota_timer);
	os_timer_setfn(&ota_timer, (os_timer_func_t *)connect_timeout_cb, 0);
	os_timer_arm(&ota_timer, OTA_NETWORK_TIMEOUT, 0);
}

// start the flash process
bool ICACHE_FLASH_ATTR flash_start(flash_callback callback) {

	err_t result;

	// create upgrade status structure
	upgrade = (upgrade_status*)os_zalloc(sizeof(upgrade_status));
	if (!upgrade) {
		uart0_send("No ram!\r\n");
		return false;
	}

	// store the callback
	upgrade->callback = callback;

	// flash to hardcoded address
	upgrade->write_status = flash_write_init(FLASH_ADDR);

	// create connection
	upgrade->conn = (struct espconn *)os_zalloc(sizeof(struct espconn));
	if (!upgrade->conn) {
		uart0_send("No ram!\r\n");
		os_free(upgrade);
		return false;
	}
	upgrade->conn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
	if (!upgrade->conn->proto.tcp) {
		uart0_send("No ram!\r\n");
		os_free(upgrade->conn);
		os_free(upgrade);
		return false;
	}

	// dns lookup
	result = espconn_gethostbyname(upgrade->conn, OTA_HOST, &upgrade->ip, upgrade_resolved);
	if (result == ESPCONN_OK) {
		// hostname is already cached or is actually a dotted decimal ip address
		upgrade_resolved(0, &upgrade->ip, upgrade->conn);
	} else if (result == ESPCONN_INPROGRESS) {
		// lookup taking place, will call upgrade_resolved on completion
	} else {
		uart0_send("DNS error!\r\n");
		os_free(upgrade->conn->proto.tcp);
		os_free(upgrade->conn);
		os_free(upgrade);
		return false;
	}

	return true;
}

