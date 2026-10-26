/* XS1 USB device link over an external ULPI PHY (L-series, clean-room).
   Clean-room sources: USB 2.0 spec, ULPI 1.1 spec, the XS1 architecture manual
   (ports/clock blocks/channels), TinyUSB's dcd interface, and the pin map in
   docs/hardware/s-mk1.md + fw/kks/notes/xmos.md §5. No XMOS or NI source used.

   Layering:
     usb_crc.c / usb_packet.c / usb_ep.c  pure logic, host-tested
     usb_ulpi.c                           XS1 ports + clock block + SIE thread
     dcd_xs1_ulpi.c                       TinyUSB dcd_* over a channel to the thread

   The ULPI link runs in its own hardware thread (the SIE). TinyUSB runs in the
   application USB thread; the two talk over one XS1 channel (no ISRs). */
#ifndef USB_ULPI_H
#define USB_ULPI_H

#include <stdint.h>
#include "usb_ep.h"

/* ---- ULPI signal -> XS1 port map (derived here; see usb.md for the rationale).
   Pins are from fw/kks/notes/xmos.md §5: CLK on 1H; data/control/flags on
   8A/8B/8C/8D and 1N/1O/1P. A 60 MHz 8-bit ULPI link (ULPI 1.1 §3.1) needs an
   8-bit bidirectional data bus plus DIR, NXT (PHY->link) and STP (link->PHY),
   all synchronous to the PHY's 60 MHz CLK.

   Resource id = width<<16 | index<<8 (per fw/kks/notes/xmos.md §5). Indices use
   the XS1 letter order A=0..P=15; the numeric ids must be confirmed against the
   XS1-L port header for the exact die before flashing (see usb.md risks). ---- */
#define XS1_PORT(w, idx)   ((uint32_t)(((w) << 16) | ((idx) << 8)))

#define ULPI_PORT_CLK   XS1_PORT(1, 7)    /* 1H  ULPI CLK (clock source)   */
#define ULPI_PORT_DATA  XS1_PORT(8, 0)    /* 8A  DATA[7:0] (bidirectional) */
#define ULPI_PORT_DIR   XS1_PORT(1, 13)   /* 1N  DIR  (PHY -> link)        */
#define ULPI_PORT_NXT   XS1_PORT(1, 14)   /* 1O  NXT  (PHY -> link)        */
#define ULPI_PORT_STP   XS1_PORT(1, 15)   /* 1P  STP  (link -> PHY)        */
#define ULPI_CLKBLK     (1u << 8 | 6)    /* clock block 1, driven by 1H     */

/* ---- Messages between the SIE thread and the DCD (TinyUSB thread) ----
   Fixed 4-word messages (tag, a, b, c) in two queues in shared memory (same tile): commands
   DCD -> SIE, events SIE -> DCD. A producer that finds the consumer's `bell` clear sets it
   and sends one byte token on its chanend; the consumer takes the token, clears `bell`, then
   empties the queue. So at most one token is ever in flight each way and neither thread can
   block the other: with channel messages both ways, a SIE sending an event and a DCD sending
   a command at the same time would each wait for the other's buffer forever. A full queue
   waits for the consumer, which never waits for the producer. */
enum {
    /* DCD -> SIE */
    ULPI_CMD_OPEN = 1,   /* a=ep_addr b=type c=max_packet */
    ULPI_CMD_XFER,       /* a=ep_addr b=bufptr c=len      */
    ULPI_CMD_STALL,      /* a=ep_addr                     */
    ULPI_CMD_CLR_STALL,  /* a=ep_addr                     */
    ULPI_CMD_SET_ADDR,   /* a=addr                        */
    ULPI_CMD_CLOSE_ALL,
    /* SIE -> DCD */
    ULPI_EVT_SETUP = 0x40, /* a,b = 8 setup bytes */
    ULPI_EVT_COMPLETE,     /* a=ep_addr b=len     */
    ULPI_EVT_RESET,        /* a=speed             */
    ULPI_EVT_SOF,          /* a=frame             */
};

#define ULPI_QUEUE 16
typedef struct { uint32_t tag, a, b, c; } ulpi_msg_t;
typedef struct {
    volatile ulpi_msg_t m[ULPI_QUEUE];
    volatile uint32_t head, tail;   /* producer writes head, consumer tail */
    volatile uint32_t bell;         /* a token is on its way to the consumer */
} ulpi_queue_t;

extern ulpi_queue_t ulpi_cmdq, ulpi_evq;     /* dcd_xs1_ulpi.c */
/* producer side: `chan` is the producer's chanend (connected to the consumer's) */
void ulpi_put(ulpi_queue_t *q, uint32_t chan, uint32_t tag, uint32_t a, uint32_t b, uint32_t c);
/* consumer side, after taking the bell token: 1 = a message */
int ulpi_get(ulpi_queue_t *q, ulpi_msg_t *m);

/* SIE thread entry. `chan` is this thread's chanend (connected to the DCD's). Owns the ULPI
   ports and the usb_ep state; never returns. */
void usb_ulpi_sie_thread(uint32_t chan);

/* Reset and initialise the PHY to full speed. Returns FUNC_CTRL read back.
   Split out so it can run once before the SIE loop. */
uint8_t usb_ulpi_phy_bringup(void);

/* dcd_xs1_ulpi.c runs in the TinyUSB thread. `chan` is its chanend, connected to the
   SIE's. Call once before tusb_init(). */
void usb_dcd_bind_channel(uint32_t chan);

/* The TinyUSB thread waits on its chanend (with a timeout) and, when a bell token is
   there, calls this: it takes the token and feeds every queued SIE event to the stack. */
void usb_dcd_pump(void);

#endif
