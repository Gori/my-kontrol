#include "usb_ep.h"

#define DIR_OF(a)  (((a) & 0x80) ? 1 : 0)
#define NUM_OF(a)  ((a) & 0x0F)

static usb_ep_t *ep_of(usb_ep_ctx_t *c, uint8_t ep_addr)
{
    uint8_t n = NUM_OF(ep_addr);
    if (n >= USB_EP_NUM)
        return NULL;
    return &c->ep[n][DIR_OF(ep_addr)];
}

void usb_ep_init(usb_ep_ctx_t *c)
{
    for (int n = 0; n < USB_EP_NUM; n++)
        for (int d = 0; d < 2; d++)
            c->ep[n][d] = (usb_ep_t){0};
    c->address = 0;
    c->pending_address = 0;
    c->has_pending_address = 0;
    /* EP0 OUT/IN are always open at 64 bytes (control). */
    c->ep[0][0] = (usb_ep_t){ .configured = 1, .type = USB_XFER_CONTROL, .max_packet = 64 };
    c->ep[0][1] = (usb_ep_t){ .configured = 1, .type = USB_XFER_CONTROL, .max_packet = 64 };
}

void usb_ep_bus_reset(usb_ep_ctx_t *c)
{
    usb_ep_init(c);
}

void usb_ep_close_all(usb_ep_ctx_t *c)
{
    for (int n = 1; n < USB_EP_NUM; n++)
        for (int d = 0; d < 2; d++)
            c->ep[n][d] = (usb_ep_t){0};
}

int usb_ep_open(usb_ep_ctx_t *c, uint8_t ep_addr, usb_xfer_type_t type, uint16_t max_packet)
{
    usb_ep_t *e = ep_of(c, ep_addr);
    if (!e)
        return 0;
    *e = (usb_ep_t){ .configured = 1, .type = (uint8_t)type, .max_packet = max_packet };
    return 1;
}

int usb_ep_xfer(usb_ep_ctx_t *c, uint8_t ep_addr, uint8_t *buf, uint16_t len)
{
    usb_ep_t *e = ep_of(c, ep_addr);
    if (!e || !e->configured)
        return 0;
    e->buf = buf;
    e->total = len;
    e->done = 0;
    e->active = 1;
    return 1;
}

void usb_ep_stall(usb_ep_ctx_t *c, uint8_t ep_addr)
{
    usb_ep_t *e = ep_of(c, ep_addr);
    if (e) {
        e->stalled = 1;
        e->active = 0;
    }
}

void usb_ep_clear_stall(usb_ep_ctx_t *c, uint8_t ep_addr)
{
    usb_ep_t *e = ep_of(c, ep_addr);
    if (e) {
        e->stalled = 0;
        e->toggle = 0;        /* data toggle resets to DATA0 */
    }
}

void usb_ep_set_address(usb_ep_ctx_t *c, uint8_t addr)
{
    c->pending_address = addr;
    c->has_pending_address = 1;
    /* Status stage: EP0-IN zero-length packet, sent at the old address. */
    usb_ep_xfer(c, 0x80, NULL, 0);
}

void usb_ep_on_setup(usb_ep_ctx_t *c, const uint8_t setup[8])
{
    /* A SETUP always clears a stale EP0 stall and restarts the toggle.
       Control data/status stages begin with DATA1 (USB 2.0 §8.5.3). */
    usb_ep_t *o = &c->ep[0][0], *i = &c->ep[0][1];
    o->stalled = i->stalled = 0;
    o->active  = i->active  = 0;
    o->toggle  = i->toggle  = 1;
    if (c->on_setup)
        c->on_setup(c->user, setup);
}

usb_resp_t usb_ep_on_in(usb_ep_ctx_t *c, uint8_t epnum)
{
    usb_resp_t r = { USB_RESP_NONE, NULL, 0, USB_PID_DATA0 };
    if (epnum >= USB_EP_NUM)
        return r;
    usb_ep_t *e = &c->ep[epnum][1];
    if (!e->configured)
        return r;
    if (e->stalled)  { r.kind = USB_RESP_STALL; return r; }
    if (!e->active)  { r.kind = USB_RESP_NAK;   return r; }

    uint16_t remaining = e->total - e->done;
    uint16_t chunk = remaining < e->max_packet ? remaining : e->max_packet;
    r.kind = USB_RESP_DATA;
    r.data = e->buf ? e->buf + e->done : NULL;
    r.len  = chunk;
    r.pid  = e->toggle ? USB_PID_DATA1 : USB_PID_DATA0;

    /* Model the host ACK: advance and flip the toggle. The real SIE only
       advances after the ACK handshake and re-sends on a missing ACK. */
    e->done = (uint16_t)(e->done + chunk);
    e->toggle ^= 1;

    if (chunk < e->max_packet || e->done == e->total) {
        e->active = 0;
        if (epnum == 0 && c->has_pending_address) {
            /* EP0-IN status of SET_ADDRESS just completed: latch it now. */
            c->address = c->pending_address;
            c->has_pending_address = 0;
            if (c->on_address)
                c->on_address(c->user, c->address);
        }
        if (c->on_complete)
            c->on_complete(c->user, (uint8_t)(0x80 | epnum), e->done);
    }
    return r;
}

usb_resp_t usb_ep_on_out(usb_ep_ctx_t *c, uint8_t epnum, const uint8_t *data, uint16_t len)
{
    usb_resp_t r = { USB_RESP_NONE, NULL, 0, USB_PID_DATA0 };
    if (epnum >= USB_EP_NUM)
        return r;
    usb_ep_t *e = &c->ep[epnum][0];
    if (!e->configured)
        return r;
    if (e->stalled) { r.kind = USB_RESP_STALL; return r; }
    if (!e->active) { r.kind = USB_RESP_NAK;   return r; }

    uint16_t space = e->total - e->done;
    uint16_t n = len < space ? len : space;
    if (e->buf)
        for (uint16_t k = 0; k < n; k++)
            e->buf[e->done + k] = data[k];
    e->done = (uint16_t)(e->done + n);
    e->toggle ^= 1;
    r.kind = USB_RESP_ACK;

    if (len < e->max_packet || e->done >= e->total) {   /* short packet or full */
        e->active = 0;
        if (c->on_complete)
            c->on_complete(c->user, (uint8_t)epnum, e->done);
    }
    return r;
}
