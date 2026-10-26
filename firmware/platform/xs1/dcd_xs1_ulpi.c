/* TinyUSB device controller driver (dcd_*) for the XS1 ULPI link.
   Clean-room: implements the interface in firmware/lib/tinyusb/src/device/dcd.h
   over the SIE thread in usb_ulpi.c. No ISR: the SIE posts events in a shared queue and
   rings a one-token bell (usb_ulpi.h); this file, in the TinyUSB thread, pumps them into
   the stack, and posts commands the same way.

   Select this DCD by building the XS1 board with CFG_TUSB_MCU = OPT_MCU_NONE so
   TinyUSB links these symbols instead of a built-in controller driver. */
#include "tusb_option.h"

#if CFG_TUD_ENABLED

#include "device/dcd.h"
#include "usb_ulpi.h"
#include "bringup.h"

#if defined(__xcore__)
static inline void c_outt(uint32_t c, uint32_t t) { asm volatile("outt res[%0], %1" :: "r"(c), "r"(t) : "memory"); }
static inline uint32_t c_int(uint32_t c)           { uint32_t t; asm volatile("int %0, res[%1]" : "=r"(t) : "r"(c) : "memory"); return t; }
#else
/* Host builds of this file (compile check only): no channel. */
static inline void c_outt(uint32_t c, uint32_t t) { (void)c; (void)t; }
static inline uint32_t c_int(uint32_t c)           { (void)c; return 0; }
#endif

ulpi_queue_t ulpi_cmdq, ulpi_evq;

void ulpi_put(ulpi_queue_t *q, uint32_t chan, uint32_t tag, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = q->head;
    while (h - q->tail >= ULPI_QUEUE) { }      /* full: the consumer never waits for us */
    volatile ulpi_msg_t *m = &q->m[h % ULPI_QUEUE];
    m->tag = tag, m->a = a, m->b = b, m->c = c;
    q->head = h + 1;
    if (!q->bell) {
        q->bell = 1;
        c_outt(chan, 0);
    }
}

int ulpi_get(ulpi_queue_t *q, ulpi_msg_t *out)
{
    uint32_t t = q->tail;
    if (t == q->head)
        return 0;
    volatile ulpi_msg_t *m = &q->m[t % ULPI_QUEUE];
    out->tag = m->tag, out->a = m->a, out->b = m->b, out->c = m->c;
    q->tail = t + 1;
    return 1;
}

static uint32_t s_chan;

void usb_dcd_bind_channel(uint32_t chan) { s_chan = chan; }

static void send_cmd(uint32_t tag, uint32_t a, uint32_t b, uint32_t cc)
{
    ulpi_put(&ulpi_cmdq, s_chan, tag, a, b, cc);
}

//--------------------------------------------------------------------+
// Controller API
//--------------------------------------------------------------------+

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    (void)rhport; (void)rh_init;    /* the SIE thread brings up the PHY */
    return true;
}

void dcd_int_enable(uint8_t rhport)  { (void)rhport; }
void dcd_int_disable(uint8_t rhport) { (void)rhport; }

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
    (void)rhport;
    /* The SIE sends the EP0-IN status ZLP and latches the address after it
       completes (USB 2.0 §9.4.6). */
    send_cmd(ULPI_CMD_SET_ADDR, dev_addr, 0, 0);
    BRINGUP_DIAG(bringup_diag.address = dev_addr; bringup_diag.flags |= 4);
}

void dcd_remote_wakeup(uint8_t rhport) { (void)rhport; }
void dcd_connect(uint8_t rhport)       { (void)rhport; }
void dcd_disconnect(uint8_t rhport)    { (void)rhport; }
void dcd_sof_enable(uint8_t rhport, bool en) { (void)rhport; (void)en; }

/* Take the bell token and pump every queued SIE event into the stack (TinyUSB thread). */
void usb_dcd_pump(void)
{
    ulpi_msg_t m;
    (void)c_int(s_chan);
    ulpi_evq.bell = 0;
    while (ulpi_get(&ulpi_evq, &m)) {
        switch (m.tag) {
        case ULPI_EVT_SETUP: {
            uint8_t s[8];
            s[0]=m.a; s[1]=m.a>>8; s[2]=m.a>>16; s[3]=m.a>>24;
            s[4]=m.b; s[5]=m.b>>8; s[6]=m.b>>16; s[7]=m.b>>24;
            dcd_event_setup_received(0, s, false);
            BRINGUP_DIAG(bringup_diag.evt_setup++; bringup_diag.flags |= 2);
            break;
        }
        case ULPI_EVT_COMPLETE:
            dcd_event_xfer_complete(0, (uint8_t)m.a, m.b, XFER_RESULT_SUCCESS, false);
            BRINGUP_DIAG(bringup_diag.evt_complete++);
            break;
        case ULPI_EVT_RESET:
            dcd_event_bus_reset(0, (tusb_speed_t)m.a, false);
            BRINGUP_DIAG(bringup_diag.evt_reset++; bringup_diag.flags |= 1);
            break;
        case ULPI_EVT_SOF:
            dcd_event_sof(0, m.a, false);
            break;
        default:
            break;
        }
    }
}

void dcd_int_handler(uint8_t rhport) { (void)rhport; usb_dcd_pump(); }

//--------------------------------------------------------------------+
// Endpoint API
//--------------------------------------------------------------------+

void dcd_edpt0_status_complete(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport; (void)request;
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *desc_ep)
{
    (void)rhport;
    uint32_t mps  = desc_ep->wMaxPacketSize & 0x7FF;
    uint32_t type = desc_ep->bmAttributes.xfer;
    send_cmd(ULPI_CMD_OPEN, desc_ep->bEndpointAddress, type, mps);
    return true;
}

void dcd_edpt_close_all(uint8_t rhport)
{
    (void)rhport;
    send_cmd(ULPI_CMD_CLOSE_ALL, 0, 0, 0);
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr)
{
    (void)rhport; (void)is_isr;
    send_cmd(ULPI_CMD_XFER, ep_addr, (uint32_t)(uintptr_t)buffer, total_bytes);
    BRINGUP_DIAG(bringup_diag.dcd_xfers++);
    return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    send_cmd(ULPI_CMD_STALL, ep_addr, 0, 0);
    BRINGUP_DIAG(bringup_diag.dcd_stalls++);
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    send_cmd(ULPI_CMD_CLR_STALL, ep_addr, 0, 0);
}

bool dcd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size)
{
    (void)rhport; (void)ep_addr; (void)largest_packet_size;
    return false;   /* no isochronous endpoints on this device */
}

bool dcd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const *desc_ep)
{
    (void)rhport; (void)desc_ep;
    return false;
}

#endif
