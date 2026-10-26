#pragma once
/* Tile 1's DFU backend (usb/dfu_xmos.h): the upgrade image slot (upgrade.c) on the SPI
   flash, and the reboot the platform does. Host-tested with the USB stack (test_usbdev.c). */
#include "dfu_xmos.h"
#include "spi_flash.h"

const dfu_backend_t *tile1_dfu_backend(const spi_bus_t *bus, int chip, void (*reboot)(int into_dfu));
