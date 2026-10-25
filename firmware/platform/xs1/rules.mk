# XS1 link rules (included by the Makefile after OBJS): one RAM image per tile, and their
# maps. The upgrade image is packaged with XMOS's xflash; tools/xcore/s49run.py runs the
# two tile images on the board emulator.
TILE1_OBJS = $(patsubst %.c,$(BUILD)/tile1/%.o,$(TILE1_SRCS))

$(BUILD)/tile1/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/tile0.bin: $(OBJS) platform/xs1/tile.ld platform/xs1/crt0.s
	python3 $(XCORE)/xld.py @platform/xs1/tile.ld $(OBJS) -o $@ -m $(BUILD)/tile0.map

$(BUILD)/tile1.bin: $(TILE1_OBJS) platform/xs1/tile.ld platform/xs1/crt0.s
	python3 $(XCORE)/xld.py @platform/xs1/tile.ld $(TILE1_OBJS) -o $@ -m $(BUILD)/tile1.map

$(OUT).bin: $(BUILD)/tile0.bin $(BUILD)/tile1.bin
	@echo "$(BUILD)/tile0.bin $(BUILD)/tile1.bin (run: ../tools/xcore/s49run.py; package: xflash)"

-include $(TILE1_OBJS:.o=.d)
