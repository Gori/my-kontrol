#pragma once
/* The USB device of a keyboard whose core runs elsewhere (XS1 tile 1 for the S MK1: the
   core is on tile 0, behind the tile link). TinyUSB, our descriptors, and queues that the
   link side (platform/xs1/tile1.c) fills and empties:

   - normal device (BOARD_USB_PID): MIDI with three cables, as the core numbers them
     (core/midi.h): 0 "My Kontrol Keys", 1 "My Kontrol DAW", 2 "My Kontrol MIDI" (the 5-pin
     EXT port); a HID keyboard (the 4D encoder's arrow keys, as on the M32); and a DFU
     runtime interface (usb/dfu_xmos.c). Interfaces in NI's order: 0 audio control, 1 MIDI
     streaming, 2 HID, 3 DFU, so NI's updater's RESETINTODFU (wIndex 3) reaches it.
   - DFU mode (BOARD_USB_PID_DFU): only the DFU interface. Uses nothing from the link: it
     comes up whatever tile 0 does.

   Queues are single producer, single consumer, between two threads of one tile. */
#include <stdint.h>
#include "dfu_xmos.h"

#define USB_LINK_RING 64
typedef struct {
    volatile uint32_t buf[USB_LINK_RING];
    volatile uint32_t head, tail;
} usb_ring_t;

int usb_ring_put(usb_ring_t *r, uint32_t v);      /* 0 = full */
int usb_ring_get(usb_ring_t *r, uint32_t *v);     /* 0 = empty */

/* USB-MIDI event packets as p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24 */
extern usb_ring_t usb_midi_from_host;   /* USB thread -> link */
extern usb_ring_t usb_midi_to_host;     /* link -> USB thread */
extern usb_ring_t usb_kbd_taps;         /* link -> USB thread: modifier | key << 8 */
extern usb_ring_t usb_dev_events;       /* USB thread -> link (DEV builds): type | id << 8 | value << 16 */
extern volatile uint8_t usb_link_mounted;

/* id: 12 bytes for the serial number string */
void usb_link_init(int dfu_mode, const uint8_t id[12], const dfu_backend_t *dfu);
void usb_link_task(void);               /* USB thread: TinyUSB and the queues */
void usb_link_notify(void);             /* the platform: wake the link side (a queue grew,
                                           usb_link_mounted changed) */
