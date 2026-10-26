#pragma once
/* TinyUSB on XS1 tile 1 (usb/usb_link.c): our DCD (platform/xs1/dcd_xs1_ulpi.c), full speed.
   The DFU interface is our own class driver (usb/dfu_xmos.c), not TinyUSB's. */
#define CFG_TUSB_MCU           OPT_MCU_NONE
#define CFG_TUSB_OS            OPT_OS_NONE
#define CFG_TUSB_DEBUG         0
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED        1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUP_DCD_ENDPOINT_MAX   8      /* usb_ep.h USB_EP_NUM */

#define CFG_TUD_MIDI           1
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 128

#define CFG_TUD_HID            1
#define CFG_TUD_HID_EP_BUFSIZE 8
