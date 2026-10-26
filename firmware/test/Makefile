# Host tests: no hardware needed.
#   make            build and run all tests
#   make preview    render every screen to out/*.pbm (and out/*.png if Pillow is installed)

CC ?= cc
CFLAGS = -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
         -Ihost -I../core -I../usb
BIN = build
CORE = ../core

all: $(BIN)/test_smartplay $(BIN)/test_core $(BIN)/test_host $(BIN)/test_midi_din $(BIN)/test_midi_din0 $(BIN)/test_usb \
     $(BIN)/test_seg $(BIN)/test_panel $(BIN)/test_flash $(BIN)/test_upgrade $(BIN)/test_usbdev
	$(BIN)/test_smartplay
	$(BIN)/test_core
	$(BIN)/test_host
	$(BIN)/test_midi_din
	$(BIN)/test_midi_din0
	$(BIN)/test_usb
	$(BIN)/test_seg
	$(BIN)/test_panel
	$(BIN)/test_flash
	$(BIN)/test_upgrade
	$(BIN)/test_usbdev

$(BIN):
	mkdir -p $@

$(BIN)/test_smartplay: test_smartplay.c $(CORE)/smartplay.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/test_core: test_core.c host/fake_hal.c $(CORE)/midi.c $(CORE)/play.c $(CORE)/smartplay.c \
                  $(CORE)/settings.c $(CORE)/settings_store.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/test_host: test_host.c host/fake_hal.c $(CORE)/host.c $(CORE)/mcu.c $(CORE)/midi.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

# XS1 ULPI USB driver, pure logic only (no ports): CRCs, framing, ULPI register
# cycle, DCD endpoint/enumeration state machine.
XS1 = ../platform/xs1
$(BIN)/test_usb: test_usb.c $(XS1)/usb_crc.c $(XS1)/usb_packet.c $(XS1)/usb_ep.c | $(BIN)
	$(CC) $(CFLAGS) -I$(XS1) $^ -o $@

# S-series displays: 16-segment font, ST7565 segment map, the seg9 renderer
$(BIN)/test_seg: test_seg.c ../drivers/font16.c ../drivers/st7565_seg.c ../ui/seg9.c | $(BIN)
	$(CC) $(CFLAGS) -I../drivers $^ -o $@

# S-series panel drivers: LED chains, keybed messages and velocity, sensor MCUs, light guide
$(BIN)/test_panel: test_panel.c ../drivers/shift595.c ../drivers/ni_keybed.c ../drivers/ni_sensor.c \
                   $(CORE)/lightguide.c | $(BIN)
	$(CC) $(CFLAGS) -I../drivers $^ -o $@

# S-series SPI flash: command sequences, protection, the guard below NI's data
$(BIN)/test_flash: test_flash.c ../drivers/spi_flash.c | $(BIN)
	$(CC) $(CFLAGS) -I../drivers $^ -o $@

# S-series DFU target: the upgrade image slot of the XMOS boot flash
$(BIN)/test_upgrade: test_upgrade.c host/fake_flash.c $(XS1)/upgrade.c ../drivers/spi_flash.c | $(BIN)
	$(CC) $(CFLAGS) -I../drivers -I$(XS1) $^ -o $@

# S-series USB device on tile 1: TinyUSB as built for it, descriptors, MIDI/HID queues, DFU
# with the upgrade slot, through a fake DCD (the S49's board_config.h, tile 1's tusb_config.h)
TUSB_DIR = ../lib/tinyusb/src
$(BIN)/test_usbdev: test_usbdev.c host/fake_flash.c ../usb/usb_link.c ../usb/dfu_xmos.c $(XS1)/tile1_dfu.c \
                    $(XS1)/upgrade.c ../drivers/spi_flash.c $(TUSB_DIR)/tusb.c $(TUSB_DIR)/common/tusb_fifo.c \
                    $(TUSB_DIR)/device/usbd.c $(TUSB_DIR)/class/midi/midi_device.c $(TUSB_DIR)/class/hid/hid_device.c | $(BIN)
	$(CC) -I../boards/s-mk1/variants/s49 -I$(XS1)/tusb $(CFLAGS) -I../drivers -I$(XS1) -I$(TUSB_DIR) \
	    -Wno-unused-parameter -fno-sanitize=alignment $^ -o $@   # TinyUSB reads unaligned on ARM hosts

APP_SRCS = host/fake_hal.c $(filter-out $(CORE)/main.c,$(wildcard $(CORE)/*.c)) ../ui/oled.c

# the whole core, with 5-pin MIDI and without (the M32)
$(BIN)/test_midi_din: test_midi_din.c $(APP_SRCS) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/test_midi_din0: test_midi_din.c $(APP_SRCS) | $(BIN)
	$(CC) $(CFLAGS) -DBOARD_MIDI_DIN=0 $^ -o $@

PREVIEW_SRCS = ui_preview.c $(APP_SRCS)

$(BIN)/ui_preview: $(PREVIEW_SRCS) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

preview: $(BIN)/ui_preview
	mkdir -p out
	rm -f out/*.pbm out/*.png
	$(BIN)/ui_preview out
	-python3 tools/pbm2png.py out

clean:
	rm -rf $(BIN) out

.PHONY: all preview clean
