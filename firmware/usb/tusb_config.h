#pragma once

#define CFG_TUSB_MCU           OPT_MCU_STM32F1
#define CFG_TUSB_OS            OPT_OS_NONE
#define CFG_TUSB_DEBUG         0
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED        1
#define CFG_TUD_ENDPOINT0_SIZE 64

/* Only the DFU runtime interface, so m32dfu.py `enter` (DFU_DETACH) can get back
   to the NI bootloader once the boot flag has been disarmed. */
#define CFG_TUD_DFU_RUNTIME    1

#define CFG_TUD_MIDI           1
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 128
