# M32 simulator for macOS: the firmware core on CoreMIDI virtual ports.
#   make            build/mk-sim
#   make probe      build/mk-probe, a minimal host that checks HI / HELLO / DESCRIBE
#   make check      run mk-sim headless and probe it
#   make check-stdio  scripted session over mk-sim --stdio --fake-time (no CoreMIDI)

CC = cc
CFLAGS = -O2 -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -I. -I../core
LDLIBS = -framework CoreMIDI -framework CoreFoundation
BUILD = build

CORE_SRCS = $(filter-out ../core/main.c,$(wildcard ../core/*.c))
SIM_SRCS = sim.c sim_hal.c sim_midi.c
SURFACE = $(BUILD)/surface_json.c
OBJS = $(patsubst ../core/%.c,$(BUILD)/core/%.o,$(CORE_SRCS)) $(patsubst %.c,$(BUILD)/%.o,$(SIM_SRCS)) \
       $(BUILD)/surface_json.o

all: $(BUILD)/mk-sim

$(BUILD)/core/%.o: ../core/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SURFACE): ../boards/m32/surface.json ../../tools/surface2c.py
	@mkdir -p $(dir $@)
	python3 ../../tools/surface2c.py $< $@

$(BUILD)/surface_json.o: $(SURFACE)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mk-sim: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/mk-probe: probe.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

probe: $(BUILD)/mk-probe

check: $(BUILD)/mk-sim $(BUILD)/mk-probe
	@$(BUILD)/mk-sim --no-ui --log $(BUILD)/check.log & pid=$$!; \
	$(BUILD)/mk-probe; status=$$?; kill $$pid; wait $$pid; exit $$status

check-stdio: $(BUILD)/mk-sim
	sh check_stdio.sh $(BUILD)/mk-sim

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all probe check check-stdio clean
