#pragma once

#define CFG_TUSB_MCU           OPT_MCU_STM32F1
#define CFG_TUSB_OS            OPT_OS_NONE
#define CFG_TUSB_DEBUG         0
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED        1
#define CFG_TUD_ENDPOINT0_SIZE 64

/* DFU runtime: DFU_DETACH from tools/mkflash.py or the web installer drops
   into NI's bootloader for an update. */
#define CFG_TUD_DFU_RUNTIME    1

#define CFG_TUD_MIDI           1
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 128

/* USB keyboard: the 4D encoder sends arrow keys and Return to the focused
   window (regions, notes, browser lists) */
#define CFG_TUD_HID            1
#define CFG_TUD_HID_EP_BUFSIZE 8
