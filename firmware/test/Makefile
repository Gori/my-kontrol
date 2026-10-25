# Host tests: no hardware needed.
#   make            build and run all tests
#   make preview    render every screen to out/*.pbm (and out/*.png if Pillow is installed)

CC ?= cc
CFLAGS = -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
         -Ihost -I../core -I../usb
BIN = build
CORE = ../core

all: $(BIN)/test_smartplay $(BIN)/test_core $(BIN)/test_host $(BIN)/test_midi_din $(BIN)/test_midi_din0 $(BIN)/test_usb \
     $(BIN)/test_seg
	$(BIN)/test_smartplay
	$(BIN)/test_core
	$(BIN)/test_host
	$(BIN)/test_midi_din
	$(BIN)/test_midi_din0
	$(BIN)/test_usb
	$(BIN)/test_seg

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
