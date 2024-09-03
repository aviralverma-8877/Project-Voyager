# Project-Voyager
Project-Voyager is an ESP32-LORA based application that allows users to send and recieve files over lora.
This is an ongoing project with below objectives.
- Allow single file to be shared between 2 devices (With and without encryption).
- Allow broadcasting single file to multiple devices.
- Allow syncing a directory with all its files between 2 devices. (With and without encryption)
- Allow broadcasting a directory with all its files between 2 devices.

Although Lora based communications are very slow, but this project will be usefull for sharing small documents such as,
PDFs, Word Files or Excel sheels etc. without relying on any internet or third party network.

## Flash instruction using esptools:
```
esptool.py --after hard_reset write_flash 0x1000 bootloader.bin 0x8000 partitions.bin 0xE000 boot_app0.bin 0x10000 firmware.bin 0x310000 spiffs.bin
```