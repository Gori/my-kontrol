# Dependencies: see lib/README
CROSS  = arm-none-eabi-
CC     = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE   = $(CROSS)size

TUSB   = lib/tinyusb/src

SRCS = main.c usb.c display.c gfx.c ui.c app.c settings.c smartplay.c settings_store.c mcu.c input.c knobs.c i2c_bb.c leds.c touch.c strips.c pedal.c libc.c \
       $(TUSB)/tusb.c \
       $(TUSB)/common/tusb_fifo.c \
       $(TUSB)/device/usbd.c \
       $(TUSB)/class/dfu/dfu_rt_device.c \
       $(TUSB)/class/midi/midi_device.c \
       $(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c \
       $(TUSB)/portable/st/stm32_fsdev/fsdev_common.c

CFLAGS = -mcpu=cortex-m3 -mthumb -Os -g -std=gnu11 \
         -Wall -Wextra -Wno-missing-field-initializers -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
         -DSTM32F103xB \
         -I. -Ilibc -I$(TUSB) \
         -Ilib/cmsis_device_f1/Include -Ilib/CMSIS_6/CMSIS/Core/Include \
         $(EXTRA_CFLAGS)
LDFLAGS = -nostdlib -T m32.ld -Wl,--gc-sections -Wl,-Map=build/m32.map

OBJS = $(patsubst %.c,build/%.o,$(notdir $(SRCS)))
vpath %.c $(sort $(dir $(SRCS)))

all: build/m32.bin

build/%.o: %.c tusb_config.h *.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/m32.elf: $(OBJS) m32.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@
	$(SIZE) $@

build/m32.bin: build/m32.elf
	$(OBJCOPY) -O binary $< $@

build:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean
