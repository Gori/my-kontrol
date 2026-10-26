# My Kontrol firmware.
#   make                  release build for BOARD (default m32)
#   make DEV=1            developer build: event injection over USB (tools/mktest.py)
#   make BOARD=<name>     another keyboard, see docs/porting.md
#   make BOARD=s49 HYBRID=1 DEV=1   S MK1 bring-up: tile 0 for NI's tile 1 (platform/xs1/ni_link.c)
#   make test             host tests (no hardware)
#   make sim              macOS simulator on CoreMIDI virtual ports (sim/README.md)
# Dependencies: git submodules in lib/ (git submodule update --init), python3

BOARD ?= m32
# a board is boards/<board>/ or a size variant boards/<family>/variants/<board>/,
# whose board.mk includes the family's (docs/architecture.md)
BOARD_DIR := $(firstword $(wildcard boards/$(BOARD)) $(wildcard boards/*/variants/$(BOARD)))
ifeq ($(BOARD_DIR),)
$(error no board $(BOARD): see boards/)
endif
include $(BOARD_DIR)/board.mk
include platform/$(PLATFORM)/platform.mk
BOARD_INCLUDES ?= -I$(BOARD_DIR)

BUILD  = build/$(BOARD)$(if $(HYBRID),-hybrid)$(if $(DEV),-dev)
TUSB   = lib/tinyusb/src

# a platform without TinyUSB on the core's MCU (xs1: USB runs on the other tile) sets
# PLATFORM_USB_SRCS to its own usb_init/usb_task and NO_TINYUSB = 1
CORE_SRCS = $(wildcard core/*.c) $(if $(PLATFORM_USB_SRCS),$(PLATFORM_USB_SRCS),usb/usb_device.c)
# the renderer for this board's display (core/render.h); a board.mk may override
# BOARD_UI_SRCS, e.g. ui/seg9.c for the S-series. Default: the 128x32 OLED.
UI_SRCS = $(if $(BOARD_UI_SRCS),$(BOARD_UI_SRCS),ui/oled.c)
TUSB_SRCS = $(if $(NO_TINYUSB),,$(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
            $(TUSB)/class/dfu/dfu_rt_device.c $(TUSB)/class/midi/midi_device.c $(TUSB)/class/hid/hid_device.c \
            $(PLATFORM_TUSB_SRCS))
SURFACE = $(BUILD)/surface_json.c
SRCS = $(CORE_SRCS) $(UI_SRCS) $(BOARD_SRCS) $(PLATFORM_SRCS) $(TUSB_SRCS) $(SURFACE)

VERSION := $(shell sed -n 's/.*MK_VERSION "\(.*\)"/\1/p' core/version.h)

OPT ?= -Os                          # a platform.mk may pick another, e.g. -Oz
CFLAGS = $(PLATFORM_CFLAGS) $(OPT) -g -std=gnu11 \
         -Wall -Wextra -Wno-missing-field-initializers -ffreestanding -fno-builtin \
         -ffunction-sections -fdata-sections \
         -Icore -Iusb -Iui -Idrivers $(BOARD_INCLUDES) -Iplatform/$(PLATFORM) $(PLATFORM_INCLUDES) -I$(TUSB) \
         $(if $(DEV),-DMK_DEV) $(EXTRA_CFLAGS)
LDFLAGS = -nostdlib -T $(BOARD_DIR)/$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

OBJS = $(patsubst %.c,$(BUILD)/obj/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)
OUT  = $(BUILD)/my-kontrol-$(BOARD)-$(VERSION)$(if $(DEV),-dev)

all: $(OUT).bin

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SURFACE): $(BOARD_DIR)/surface.json ../tools/surface2c.py
	@mkdir -p $(dir $@)
	python3 ../tools/surface2c.py $< $@

ifeq ($(PLATFORM_RULES),)
$(OUT).elf: $(OBJS) $(BOARD_DIR)/$(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@
	$(SIZE) $@

$(OUT).bin: $(OUT).elf
	$(OBJCOPY) -O binary $< $@
	@cp $@ $(BUILD)/firmware.bin
	@echo "$@"
else
include platform/$(PLATFORM)/rules.mk      # the platform links and packages itself
endif

test:
	$(MAKE) -C test

sim:
	$(MAKE) -C sim

clean:
	rm -rf build

-include $(DEPS)
.PHONY: all test sim clean
