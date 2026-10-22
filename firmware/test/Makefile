# Host tests: no hardware needed.
#   make            build and run all tests
#   make preview    render every screen to out/*.pbm (and out/*.png if Pillow is installed)

CC ?= cc
CFLAGS = -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
         -Ihost -I../core -I../usb
BIN = build
CORE = ../core

all: $(BIN)/test_smartplay $(BIN)/test_core $(BIN)/test_host
	$(BIN)/test_smartplay
	$(BIN)/test_core
	$(BIN)/test_host

$(BIN):
	mkdir -p $@

$(BIN)/test_smartplay: test_smartplay.c $(CORE)/smartplay.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/test_core: test_core.c host/fake_hal.c $(CORE)/midi.c $(CORE)/play.c $(CORE)/smartplay.c \
                  $(CORE)/settings.c $(CORE)/settings_store.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/test_host: test_host.c host/fake_hal.c $(CORE)/host.c $(CORE)/mcu.c $(CORE)/midi.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@

PREVIEW_SRCS = ui_preview.c host/fake_hal.c $(filter-out $(CORE)/main.c,$(wildcard $(CORE)/*.c))

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
