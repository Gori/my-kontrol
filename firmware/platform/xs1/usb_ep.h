/* Device endpoint / control-transfer state machine (USB 2.0 §8, §9).
   Pure logic, no XS1 ports and no TinyUSB types, so it builds on the host and
   is unit-tested directly (firmware/test/test_usb.c). usb_ulpi.c feeds it token
   events from the wire; dcd_xs1_ulpi.c adapts it to TinyUSB's dcd_* calls.

   Model: the SIE delivers one transaction at a time. The caller (stack) queues
   a transfer with usb_ep_xfer; the wire side calls usb_ep_on_{setup,in,out};
   the state machine returns the handshake/data to put on the bus and raises a
   completion callback when a queued transfer finishes. Data toggle, stall,
   ZLP and the SET_ADDRESS-after-status rule live here. */
#ifndef USB_EP_H
#define USB_EP_H

#include <stddef.h>
#include <stdint.h>
#include "usb_packet.h"

#define USB_EP_NUM 8          /* endpoint numbers 0..7, each with OUT and IN */

typedef enum {
    USB_XFER_CONTROL   = 0,
    USB_XFER_ISO       = 1,
    USB_XFER_BULK      = 2,
    USB_XFER_INTERRUPT = 3,
} usb_xfer_type_t;

typedef enum {
    USB_RESP_NONE = 0,   /* no endpoint / ignore */
    USB_RESP_ACK,        /* OUT accepted */
    USB_RESP_NAK,        /* nothing queued */
    USB_RESP_STALL,      /* endpoint halted */
    USB_RESP_DATA,       /* IN: send .data/.len as a DATAx packet */
} usb_resp_kind_t;

typedef struct {
    usb_resp_kind_t kind;
    const uint8_t  *data;   /* valid when kind == USB_RESP_DATA */
    uint16_t        len;
    usb_pid_t       pid;    /* DATA0 / DATA1 */
} usb_resp_t;

typedef struct usb_ep {
    uint8_t  configured;
    uint8_t  type;          /* usb_xfer_type_t */
    uint16_t max_packet;
    uint8_t  toggle;        /* next DATAx: 0 => DATA0, 1 => DATA1 */
    uint8_t  stalled;
    uint8_t  active;        /* a transfer is queued */
    uint8_t *buf;
    uint16_t total;
    uint16_t done;
} usb_ep_t;

typedef struct usb_ep_ctx {
    usb_ep_t ep[USB_EP_NUM][2];     /* [number][0=OUT,1=IN] */
    uint8_t  address;               /* current device address (filter) */
    uint8_t  pending_address;       /* latched after SET_ADDRESS status */
    uint8_t  has_pending_address;

    /* Callbacks into the stack / adapter. */
    void (*on_setup)(void *user, const uint8_t setup[8]);
    void (*on_complete)(void *user, uint8_t ep_addr, uint16_t len);
    void (*on_address)(void *user, uint8_t addr);   /* address now active */
    void  *user;
} usb_ep_ctx_t;

void usb_ep_init(usb_ep_ctx_t *c);
void usb_ep_bus_reset(usb_ep_ctx_t *c);    /* clears config, address 0 */

/* Stack side. ep_addr bit7 = direction (0x80 = IN). */
int  usb_ep_open (usb_ep_ctx_t *c, uint8_t ep_addr, usb_xfer_type_t type, uint16_t max_packet);
void usb_ep_close_all(usb_ep_ctx_t *c);    /* keeps EP0 */
int  usb_ep_xfer (usb_ep_ctx_t *c, uint8_t ep_addr, uint8_t *buf, uint16_t len);
void usb_ep_stall(usb_ep_ctx_t *c, uint8_t ep_addr);
void usb_ep_clear_stall(usb_ep_ctx_t *c, uint8_t ep_addr);

/* SET_ADDRESS: queue the EP0-IN status ZLP and latch the address only after it
   completes (USB 2.0 §9.4.6). */
void usb_ep_set_address(usb_ep_ctx_t *c, uint8_t addr);

/* Wire side (called by the ULPI thread). epnum is 0..7. */
void       usb_ep_on_setup(usb_ep_ctx_t *c, const uint8_t setup[8]);
usb_resp_t usb_ep_on_in (usb_ep_ctx_t *c, uint8_t epnum);
usb_resp_t usb_ep_on_out(usb_ep_ctx_t *c, uint8_t epnum, const uint8_t *data, uint16_t len);

#endif
