# Komplete Kontrol S-Series MK1 (S25, S49, S61, S88): XMOS XS1, 9 segment displays.
# Included by variants/<size>/board.mk. Hardware: docs/hardware/s-mk1.md
PLATFORM = xs1
BOARD_SRCS = $(wildcard boards/s-mk1/*.c) drivers/st7565_seg.c drivers/font16.c
BOARD_UI_SRCS = ui/seg9.c
BOARD_INCLUDES = -I$(BOARD_DIR) -Iboards/s-mk1
