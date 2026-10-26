#pragma once
/* Komplete Kontrol S49 MK1: compile-time sizes and flash map used by the core.
   Hardware details: docs/hardware/s-mk1.md */
#include <stdint.h>

#define BOARD_KEYS       49
#define BOARD_KNOBS      8
#define BOARD_BASE_NOTE  36      /* lowest key C2 */
#ifdef XS1_HYBRID                /* NI's tile 1 runs the 5-pin port itself (ni_link.c) */
#define BOARD_MIDI_DIN   0
#else
#define BOARD_MIDI_DIN   1       /* 5-pin MIDI In and Out (on tile 1) */
#endif
#define BOARD_SEG_DISPLAYS 9     /* ui/seg9.c: 9 displays, 2 lines of 8 characters */
#define BOARD_GUIDE_KEYS 49      /* light guide: one RGB light per key (core/lightguide.c) */
#ifdef XS1_HYBRID                /* NI's tile 1 takes 1 packet/ms */
#define BOARD_MIDI_QLEN  64
#endif
#define BOARD_DIN_EXT_QUEUE 512  /* EXT -> 5-pin queue (core/midi.c), small: tile 0 RAM */
#define BOARD_SEEK_BUTTONS 1     /* has Rewind and Fast forward (CTL_RWD, CTL_FFW) */
/* the pixel framebuffer (core/gfx.c) is not used on this board */
#define DISP_W           128
#define DISP_H           32

/* USB identity (NI's VID/PID, so NI's updater and the web installer find it) */
#define BOARD_USB_VID    0x17CC
#define BOARD_USB_PID    0x1350
#define BOARD_USB_PID_DFU 0x1352 /* update mode (tile 1's DFU device), as NI's */
#define BOARD_USB_NAME   "S49"   /* product string "My Kontrol S49" (usb/usb_link.c) */

/* Settings flash: 4 pages of 1 KB, each at the start of a 4 KB sector of the SPI flash from
   BOARD_SPI_SETTINGS (clear of NI's areas, docs/hardware/s-mk1.md); the addresses are SPI
   flash addresses. The flash hangs off tile 1, so the core's store reads them by copy
   (BOARD_FLASH_READ) through the tile link (platform/xs1/tile0_link.c): no RAM mirror on
   tile 0. The boot log (platform/xs1/hal.c) is the sector after them. Tile 1 refuses any
   change below 0x55000. */
#define BOARD_FLASH_READ  1
#define BOARD_SPI_SETTINGS 0xF0000u
#define FLASH_PAGE        1024u
#define FLASH_PRESETS_A   (BOARD_SPI_SETTINGS + 0x0000u)
#define FLASH_PRESETS_B   (BOARD_SPI_SETTINGS + 0x1000u)
#define FLASH_SETTINGS_A  (BOARD_SPI_SETTINGS + 0x2000u)
#define FLASH_SETTINGS_B  (BOARD_SPI_SETTINGS + 0x3000u)
#define BOARD_SPI_BOOTLOG  0xF4000u
