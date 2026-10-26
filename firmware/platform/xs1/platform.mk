# XMOS XS1, 2 tiles: clang --target=xcore through tools/xcore (xcc.py, xas.py, xld.py).
# Tile 0 is the usual SRCS (core, ui, board, drivers) plus the files below; tile 1 is
# TILE1_SRCS. rules.mk links both. USB runs on tile 1, so tile 0 has no TinyUSB.
XCORE   = ../tools/xcore
CC      = python3 $(XCORE)/xcc.py

PLATFORM_CFLAGS = -DHAL_HAS_IDLE
OPT = -Oz                           # 64 KB per tile for code and data: 12% smaller than -Os
PLATFORM_INCLUDES = -Iplatform/xs1/libc
PLATFORM_SRCS = platform/xs1/platform.c platform/xs1/hal.c platform/xs1/link.c \
                platform/xs1/thread.c platform/xs1/rt.c platform/xs1/sswitch.c
PLATFORM_USB_SRCS = platform/xs1/tile0_link.c
# HYBRID=1 DEV=1: tile 0 for NI's tile 1 (USB, DFU, 5-pin), with the bring-up probe; see
# platform/xs1/ni_link.c and tools/xcore/s49pack.py
ifdef HYBRID
ifndef DEV
$(error HYBRID=1 is a developer build: add DEV=1)
endif
PLATFORM_USB_SRCS = platform/xs1/ni_link.c
PLATFORM_CFLAGS += -DXS1_HYBRID -DMK_PROBE
endif
NO_TINYUSB = 1
PLATFORM_RULES = 1
TILE1_SRCS = platform/xs1/tile1.c platform/xs1/link.c platform/xs1/thread.c platform/xs1/rt.c \
             platform/xs1/sswitch.c drivers/spi_flash.c platform/xs1/upgrade.c platform/xs1/tile1_dfu.c \
             platform/xs1/usb_ulpi.c platform/xs1/usb_ep.c platform/xs1/usb_packet.c platform/xs1/usb_crc.c \
             platform/xs1/dcd_xs1_ulpi.c usb/usb_link.c usb/dfu_xmos.c \
             $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
             $(TUSB)/class/midi/midi_device.c $(TUSB)/class/hid/hid_device.c
# tile 1's TinyUSB configuration comes first (platform/xs1/tusb/tusb_config.h)
TILE1_CFLAGS = -Iplatform/xs1/tusb
