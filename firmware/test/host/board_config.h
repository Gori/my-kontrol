#pragma once
/* Host "board" for tests: the M32's sizes plus 5-pin MIDI, flash pages in RAM. */
#include <stdint.h>

#define BOARD_KEYS   32
#define BOARD_KNOBS  8
#define DISP_W       128
#define DISP_H       32
#ifndef BOARD_MIDI_DIN
#define BOARD_MIDI_DIN 1     /* -DBOARD_MIDI_DIN=0 builds the M32 case */
#endif
#define BOARD_USB_VID 0x17CC
#define BOARD_USB_PID 0x1860

extern uint8_t fake_flash[5 * 1024];
#define FLASH_PAGE        1024u
#define FLASH_PRESETS_A   ((uintptr_t)fake_flash)
#define FLASH_PRESETS_B   ((uintptr_t)fake_flash + 1024)
#define FLASH_SETTINGS_A  ((uintptr_t)fake_flash + 2048)
#define FLASH_SETTINGS_B  ((uintptr_t)fake_flash + 3072)
