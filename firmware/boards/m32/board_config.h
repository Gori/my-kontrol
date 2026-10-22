#pragma once
/* Komplete Kontrol M32: compile-time sizes and flash map used by the core.
   Hardware details: docs/hardware/m32.md */

#define BOARD_KEYS   32
#define BOARD_KNOBS  8
#define DISP_W       128
#define DISP_H       32

/* USB identity. The NI VID/PID stays so NI's bootloader, NI's updater and
   the web installer find the keyboard in both modes. */
#define BOARD_USB_VID      0x17CC
#define BOARD_USB_PID      0x1860

/* Flash (1 KB pages). NI's bootloader occupies 0x08000000-0x08002FFF; NI's
   knob names (0x0801B800), settings/calibration (0x0801F800) and the
   bootloader flag page (0x0801FC00) are never written, so restoring the
   stock firmware gives back an untouched keyboard. */
#define FLASH_PAGE        1024u
#define FLASH_PRESETS_A   0x0801E000u
#define FLASH_PRESETS_B   0x0801E400u
#define FLASH_SETTINGS_A  0x0801E800u
#define FLASH_SETTINGS_B  0x0801F000u
