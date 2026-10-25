/* TinyUSB device controller driver (dcd_*) for the XS1 ULPI link.
   Clean-room: implements the interface in firmware/lib/tinyusb/src/device/dcd.h
   over the SIE thread in usb_ulpi.c. No ISR: the SIE posts events on a channel
   and this file (running in the TinyUSB thread) pumps them into the stack.

   Select this DCD by building the XS1 board with CFG_TUSB_MCU = OPT_MCU_NONE so
   TinyUSB links these symbols instead of a built-in controller driver. */
#include "tusb_option.h"

#if CFG_TUD_ENABLED

#include "device/dcd.h"
#include "usb_ulpi.h"

#define CT_END 0x01u    /* channel END control token */

#if defined(__xcore__)
static inline void c_out(uint32_t c, uint32_t w)   { asm volatile("out res[%0], %1"   :: "r"(c), "r"(w) : "memory"); }
static inline uint32_t c_in(uint32_t c)            { uint32_t w; asm volatile("in %0, res[%1]" : "=r"(w) : "r"(c) : "memory"); return w; }
static inline void c_outct(uint32_t c, uint32_t t) { asm volatile("outct res[%0], %1" :: "r"(c), "r"(t) : "memory"); }
static inline void c_chkct(uint32_t c, uint32_t t) { asm volatile("chkct res[%0], %1" :: "r"(c), "r"(t) : "memory"); }
#else
/* Host builds of this file (compile check only): the channel is a no-op. */
static inline void c_out(uint32_t c, uint32_t w)   { (void)c; (void)w; }
static inline uint32_t c_in(uint32_t c)            { (void)c; return 0; }
static inline void c_outct(uint32_t c, uint32_t t) { (void)c; (void)t; }
static inline void c_chkct(uint32_t c, uint32_t t) { (void)c; (void)t; }
#endif

static uint32_t s_chan;

void usb_dcd_bind_channel(uint32_t chan) { s_chan = chan; }

static void send_cmd(uint32_t tag, uint32_t a, uint32_t b, uint32_t cc)
{
    uint32_t c = s_chan;
    c_out(c, tag); c_out(c, a); c_out(c, b); c_out(c, cc); c_outct(c, CT_END);
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
}

void dcd_remote_wakeup(uint8_t rhport) { (void)rhport; }
void dcd_connect(uint8_t rhport)       { (void)rhport; }
void dcd_disconnect(uint8_t rhport)    { (void)rhport; }
void dcd_sof_enable(uint8_t rhport, bool en) { (void)rhport; (void)en; }

/* Pump one SIE event into the stack. Called by the TinyUSB thread; dcd_int_handler
   forwards to it so tud_int_handler() works too. */
void usb_dcd_pump(void)
{
    uint32_t c = s_chan;
    uint32_t tag = c_in(c), a = c_in(c), b = c_in(c), cc = c_in(c);
    c_chkct(c, CT_END);
    (void)cc;
    switch (tag) {
    case ULPI_EVT_SETUP: {
        uint8_t s[8];
        s[0]=a; s[1]=a>>8; s[2]=a>>16; s[3]=a>>24;
        s[4]=b; s[5]=b>>8; s[6]=b>>16; s[7]=b>>24;
        dcd_event_setup_received(0, s, false);
        break;
    }
    case ULPI_EVT_COMPLETE:
        dcd_event_xfer_complete(0, (uint8_t)a, b, XFER_RESULT_SUCCESS, false);
        break;
    case ULPI_EVT_RESET:
        dcd_event_bus_reset(0, (tusb_speed_t)a, false);
        break;
    case ULPI_EVT_SOF:
        dcd_event_sof(0, a, false);
        break;
    default:
        break;
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
    return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    send_cmd(ULPI_CMD_STALL, ep_addr, 0, 0);
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
