#!/bin/bash

# cleanup old files
rm gpsdo.elf gpsdo.hex

avr-gcc -Os -mmcu=atxmega16a4u -DF_CPU=25000000 -o gpsdo.elf main.c gpsdo.c
if [ $? -ne 0 ]; then exit 1; fi

avr-objcopy -j .text -j .data -O ihex gpsdo.elf gpsdo.hex
if [ $? -ne 0 ]; then exit 1; fi

# change this back to xmega64A1U for production
avrdude -v -B 1.0 -c jtag3pdi -p ATxmega16A4U -P usb -u -Uflash:w:gpsdo.hex
