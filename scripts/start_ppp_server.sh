#!/bin/sh

esptool.py --after hard_reset chip_id
sleep 10
sudo stty -F /dev/ttyUSB0 raw
sudo stty -F /dev/ttyUSB0 -a
sudo pppd /dev/ttyUSB0 115200 10.0.5.2:10.0.5.1 noauth local debug dump defaultroute nocrtscts persist maxfail 0 holdoff 1 mtu 250 mru 250
