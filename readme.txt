SDK v1.5.1+ flash write bug demo

Requires gcc for xtensa, esptool.py and sdk v1.5.1 or v1.5.2

This is based on code from rBoot, but heavily modified to make a smaller, simpler test case.
rBoot is not required for this test.

To build:
- Set appropriate environment variables, e.g.:
  export WIFI_SSID=yourssid
  export WIFI_PWD=yourpass
  export ESP_HOME=/opt/esp-open-sdk/
  export PATH=/opt/esp-open-sdk/xtensa-lx106-elf/bin:$PATH

- Set web server details for file download in rboot-ota.h e.g.:
  #define OTA_HOST "192.168.7.5"
  #define OTA_PORT 80
  #define OTA_FILE "/file.bin"

- Put file.bin in the root of your webserver (or adjust path above).

- Run 'make'

- Erase device then flash:
  firmware/rom-0x00000.bin to 0x00000
  firmware/rom-0x40000.bin to 0x40000

- Boot device, connect serial and run command:
  'crc' you will get the crc of the area to be flashed (currently blank!) 0xb7094978
  'connect' will connect to wifi

- repeat until bad crc:
  'flash' will download and flash the file, then restarts the device
  'crc' again to see if flashed correctly - crc should be 0x59d9d4de

Does not fail everytime, I had to flash with this demo about 8 times before I got the first fail.
See read.bin, this is the file read back after the bad flash, it has a block of 0x7e0 bytes
containing 0xff (erased and not written, I assume). read2.bin is another example (on the first
flash after a clean start) that contains two separate runs of 0xff blank areas.

This does not happen compiled with an sdk before v1.5.1 - flash works everytime.


