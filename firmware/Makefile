# My Kontrol firmware.
#   make                  release build for BOARD (default m32)
#   make DEV=1            developer build: event injection over USB (tools/mktest.py)
#   make BOARD=<name>     another keyboard, see docs/porting.md
#   make test             host tests (no hardware)
# Dependencies: git submodules in lib/ (git submodule update --init), python3

BOARD ?= m32
include boards/$(BOARD)/board.mk
include platform/$(PLATFORM)/platform.mk

BUILD  = build/$(BOARD)$(if $(DEV),-dev)
TUSB   = lib/tinyusb/src

CORE_SRCS = $(wildcard core/*.c) usb/usb_device.c
TUSB_SRCS = $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
            $(TUSB)/class/dfu/dfu_rt_device.c $(TUSB)/class/midi/midi_device.c $(TUSB)/class/hid/hid_device.c \
            $(PLATFORM_TUSB_SRCS)
SURFACE = $(BUILD)/surface_json.c
SRCS = $(CORE_SRCS) $(BOARD_SRCS) $(PLATFORM_SRCS) $(TUSB_SRCS) $(SURFACE)

VERSION := $(shell sed -n 's/.*MK_VERSION "\(.*\)"/\1/p' core/version.h)

CFLAGS = $(PLATFORM_CFLAGS) -Os -g -std=gnu11 \
         -Wall -Wextra -Wno-missing-field-initializers -ffreestanding -fno-builtin \
         -ffunction-sections -fdata-sections \
         -Icore -Iusb -Iboards/$(BOARD) -Iplatform/$(PLATFORM) $(PLATFORM_INCLUDES) -I$(TUSB) \
         $(if $(DEV),-DMK_DEV) $(EXTRA_CFLAGS)
LDFLAGS = -nostdlib -T boards/$(BOARD)/$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

OBJS = $(patsubst %.c,$(BUILD)/obj/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)
OUT  = $(BUILD)/my-kontrol-$(BOARD)-$(VERSION)$(if $(DEV),-dev)

all: $(OUT).bin

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SURFACE): boards/$(BOARD)/surface.json ../tools/surface2c.py
	@mkdir -p $(dir $@)
	python3 ../tools/surface2c.py $< $@

$(OUT).elf: $(OBJS) boards/$(BOARD)/$(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@
	$(SIZE) $@

$(OUT).bin: $(OUT).elf
	$(OBJCOPY) -O binary $< $@
	@cp $@ $(BUILD)/firmware.bin
	@echo "$@"

test:
	$(MAKE) -C test

clean:
	rm -rf build

-include $(DEPS)
.PHONY: all test clean
