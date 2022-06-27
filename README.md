# A ESP32 flashing utility

`esputil` is a command line tool for managing Espressif devices. It is a
replacement of `esptool.py`. `esputil` is written in C and is available as a
no-dependency static binaries for Mac, Linux, Windows:

- Windows binary: [esputil.exe](https://github.com/cpq/esputil/releases/latest/download/esputil.exe)
- Linux binary: [esputil_linux](https://github.com/cpq/esputil/releases/latest/download/esputil_linux)
- MacOS binary: [esputil_macos](https://github.com/cpq/esputil/releases/latest/download/esputil_macos)


The following `esputil` features makes it useful for dealing with ESP32:
- `esputil mkhex` command can create a single .hex file from multiple .bin
  files, which is useful for distributing ESP32 firmwares as a single
  flashable file
- `esputil unhex` command unpacks a .hex file back into a collection of .bin
  files
- `esputil flash` command can flash .hex files and .bin files


# Usage

```sh
$ esputil -h
Defaults: BAUD=115200, PORT=/dev/ttyUSB0
Usage:
  esputil [-v] [-b BAUD] [-p PORT] monitor
  esputil [-v] [-b BAUD] [-p PORT] info
  esputil [-v] [-b BAUD] [-p PORT] readmem ADDR SIZE
  esputil [-v] [-b BAUD] [-p PORT] readflash ADDR SIZE
  esputil [-v] [-b BAUD] [-p PORT] [-fp FLASH_PARAMS] [-fspi FLASH_SPI] flash ADDRESS1 BINFILE1 ...
  esputil [-v] [-b BAUD] [-p PORT] [-fp FLASH_PARAMS] [-fspi FLASH_SPI] flash FILE.HEX
  esputil [-v] mkbin FIRMWARE.ELF FIRMWARE.BIN
  esputil mkhex ADDRESS1 BINFILE1 ADDRESS2 BINFILE2 ...
  esputil [-tmp TMP_DIR] unhex HEXFILE
```

Example: flash MDK-built ESP32C3 firmware:

```sh
$ esputil flash 0 firmware.bin
```

Example: flash ESP-IDF built firmware on ESP32-PICO-Kit board:

```sh
$ esputil -fspi 6,17,8,11,16 flash 
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partitions.bin \
  0xe000 build/ota_data_initial.bin \
  0x10000 build/firmware.bin
```

# ESP32 flashing

Flashing ESP32 chips is done via UART. In order to do so, ESP32 should be
rebooted in the flashing mode, by pulling IO0 low during boot. Then, a ROM
bootloader uses SLIP framing for a simple serial protocol, which is
described at https://docs.espressif.com/projects/esptool/en/latest/advanced-topics/serial-protocol.html.

Using that SLIP protocol, it is possible to write images to flash at
any offset. That is what [tools/esputil.c](tools/esputil.c) implements.
The image should be of the following format:

- COMMON HEADER - 4 bytes, contains number of segments in the image and flash params
- ENTRY POINT ADDRESS - 4 bytes, the beginning of the image code
- EXTENDED HEADER - 16 bytes, contains chip ID and extra flash params
- One or more SEGMENTS, which are padded to 16 bytes

```
 | COMMON HEADER |  ENTRY  |           EXTENDED HEADER          | SEGM1 | ... | 
 | 0xe9 N F1 F2  | X X X X | 0xee 0 0 0 C 0 V 0 0 0 0 0 0 0 0 1 |       | ... | 

   0xe9 - Espressif image magic number. All images must start with 0xe9
   N    - a number of segments in the image
   F1   - flash mode. 0: QIO, 1: QOUT, 2: DIO, 3: DOUT
   F2   - flash size (high 4 bits) and flash frequency (low 4 bits):
            size: 0: 1MB, 0x10: 2MB, 0x20: 4MB, 0x30: 8MB, 0x40: 16MB
            freq: 0: 40m, 1: 26m, 2: 20m, 0xf: 80m
   ENTRY - 4-byte entry point address in little endian
   C     - Chip ID. 0: ESP32, 5: ESP32C3
   V     - Chip revision

   Segment format: | ADDR | SIZE | DATA |
      ADDR  - segment load address
      SIZE  - segment size, aligned to 4 bytes
      DATA  - segment data, padded with 0 to 16-byte boundary
```

## Flash parameters

Image header format includes two bytes, `F1` and `F2`, which desribe
SPI flash parameters that ROM bootloader uses to load the rest of the firmware.

- Flash mode. F1 byte, `0`: qio, `1`: qout, `2`: dio, `3`: dout
- FLash size. High 4 bits of F2 byte,
   - for ESP32: `0`: 1m, `1`: 2m, `2`: 4m, `3`: 8m, `4`: 16m
   - for ESP8266: `0`: 512k, `1`: 256k, `2`: 1m, `3`: 2m, `4`: 4m, `8`: 8m, `9`: 16m
- Flash frequency. Low 4 bits of F2 byte, `0`: 40m, `1`: 26m, `2`:
  20m, `f`: 80m

By default, `esputil` fetches flash params `F1` and `F2` from the existing
bootloader by reading first 4 bytes of the bootloader from flash.  It is
possible to manually set flash params via the `-fp 0xABC` command line flag,
where A is flash mode, B is flash size, C is flash frequency.  For example `fp
0x220` sets flash to DIO, 4MB, 40MHz:

```sh
$ esputil -fp 0x220 flash 0 firmware.bin
```

## FLash SPI pin settings

Some boards fail to talk to flash: when you attempt to `esputil flash` them,
they'll time out with the `flash_begin/erase failed`, for example trying to
flash a bootloader on a ESP32-PICO-D4-Kit:


```sh
$ esputil flash 4096 build/bootloader/bootloader.bin 
Error: can't read bootloader @ addr 0x1000
Erasing 24736 bytes @ 0x1000
flash_begin/erase failed
```

This is because ROM bootloader on such boards have wrong SPI pins settings.
Espressif's `esptool.py` alleviates that by uploading its own piece of
software into ESP32 RAM, which does the right thing. `esputil` uses ROM
bootloader, and in order to fix an issue, a `-fspi FLASH_PARAMS` parameter
can be set which manually sets flash SPI pins. The format of the 
`FLASH_PARAMS` is five comma-separated integers for CLK,Q,D,HD,CS pins.

A previously failed ESP32-PICO-D4-Kit example can be fixed by passing
a correct SPI pin settings:

```sh
$ esputil -fspi 6,17,8,11,16 flash 4096 build/bootloader/bootloader.bin 
Written build/bootloader/bootloader.bin, 24736 bytes @ 0x1000
```
