/* DFU 1.1 with XMOS's vendor requests (dfu_xmos.h). */
#include <string.h>
#include "dfu_xmos.h"
#include "class/dfu/dfu.h"

#define APP_IDLE 0
#define POLL_MS  5

static const dfu_backend_t *be;
static uint8_t mode_dfu, state, status, reboot_to;    /* reboot_to: 0 none, 1 app, 2 DFU */
static uint32_t up_off;
static uint8_t buf[DFU_XMOS_BLOCK];

void dfu_xmos_setup(const dfu_backend_t *backend, int dfu_mode)
{
    be = backend;
    mode_dfu = (uint8_t)dfu_mode;
    state = mode_dfu ? DFU_IDLE : APP_IDLE;
    status = DFU_STATUS_OK;
    reboot_to = 0;
}

uint8_t dfu_xmos_state(void)
{
    return state;
}

static void init(void) {}

static void reset(uint8_t rhport)
{
    (void)rhport;
    state = mode_dfu ? DFU_IDLE : APP_IDLE;     /* a download in progress is dropped */
    status = DFU_STATUS_OK;
}

static uint16_t open(uint8_t rhport, tusb_desc_interface_t const *d, uint16_t max_len)
{
    (void)rhport;
    if (d->bInterfaceClass != TUD_DFU_APP_CLASS || d->bInterfaceSubClass != TUD_DFU_APP_SUBCLASS ||
        d->bInterfaceProtocol != (mode_dfu ? DFU_PROTOCOL_DFU : DFU_PROTOCOL_RT))
        return 0;
    uint16_t len = 9;
    uint8_t const *p = tu_desc_next(d);
    if (max_len >= 18 && tu_desc_type(p) == DFU_DESC_FUNCTIONAL)
        len += tu_desc_len(p);
    return len;
}

static void fail(int st)
{
    status = (uint8_t)st;
    state = DFU_ERROR;
}

/* GETSTATUS also moves the state on: a block has been written by now, or the manifest
   (the image check and its header page) is done here */
static bool get_status(uint8_t rhport, tusb_control_request_t const *req)
{
    if (state == DFU_DNLOAD_SYNC) {
        state = DFU_DNLOAD_IDLE;
    } else if (state == DFU_MANIFEST_SYNC) {
        int r = be->end();
        if (r)
            fail(r);
        else
            state = DFU_IDLE;                    /* manifestation tolerant */
    }
    static uint8_t st[6];
    st[0] = status;
    st[1] = POLL_MS, st[2] = 0, st[3] = 0;
    st[4] = state;
    st[5] = 0;
    return tud_control_xfer(rhport, req, st, sizeof st);
}

static bool dfu_request(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    switch (req->bRequest) {
    case DFU_REQUEST_DNLOAD:
        if (stage == CONTROL_STAGE_SETUP) {
            if (req->wLength > sizeof buf)
                return false;
            if (req->wLength == 0) {
                if (state != DFU_DNLOAD_IDLE)
                    return false;
                state = DFU_MANIFEST_SYNC;
                return tud_control_status(rhport, req);
            }
            if (state != DFU_IDLE && state != DFU_DNLOAD_IDLE)
                return false;
            return tud_control_xfer(rhport, req, buf, req->wLength);
        }
        if (stage == CONTROL_STAGE_DATA) {
            int r = state == DFU_IDLE ? be->begin() : 0;
            if (!r)
                r = be->write(buf, req->wLength);
            if (r)
                fail(r);
            else
                state = DFU_DNLOAD_SYNC;
        }
        return true;
    case DFU_REQUEST_UPLOAD:
        if (stage != CONTROL_STAGE_SETUP)
            return true;
        if (state != DFU_IDLE && state != DFU_UPLOAD_IDLE)
            return false;
        if (state == DFU_IDLE)
            up_off = 0;
        {
            uint16_t want = req->wLength > sizeof buf ? sizeof buf : req->wLength;
            uint16_t n = be->read(up_off, buf, want);
            up_off += n;
            state = n < want ? DFU_IDLE : DFU_UPLOAD_IDLE;
            return tud_control_xfer(rhport, req, buf, n);
        }
    case DFU_REQUEST_GETSTATUS:
        return stage == CONTROL_STAGE_SETUP ? get_status(rhport, req) : true;
    case DFU_REQUEST_GETSTATE:
        return stage == CONTROL_STAGE_SETUP ? tud_control_xfer(rhport, req, &state, 1) : true;
    case DFU_REQUEST_CLRSTATUS:
        if (stage == CONTROL_STAGE_SETUP) {
            if (state != DFU_ERROR)
                return false;
            state = DFU_IDLE, status = DFU_STATUS_OK;
            return tud_control_status(rhport, req);
        }
        return true;
    case DFU_REQUEST_ABORT:
        if (stage == CONTROL_STAGE_SETUP) {
            state = DFU_IDLE, status = DFU_STATUS_OK;   /* an image without its header page */
            return tud_control_status(rhport, req);
        }
        return true;
    case DFU_XMOS_REVERTFACTORY:
        if (stage == CONTROL_STAGE_SETUP) {
            int r = be->revert();
            if (r)
                fail(r);
            return tud_control_status(rhport, req);
        }
        return true;
    case DFU_XMOS_RESETFROMDFU:
    case DFU_XMOS_RESETDEVICE:
        if (stage == CONTROL_STAGE_SETUP) {
            reboot_to = 1;
            return tud_control_status(rhport, req);
        }
        return true;
    case DFU_XMOS_SELECTIMAGE:
    case DFU_XMOS_SAVESTATE:
    case DFU_XMOS_RESTORESTATE:
        return stage == CONTROL_STAGE_SETUP ? tud_control_status(rhport, req) : true;
    default:
        return false;
    }
}

static bool runtime_request(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    switch (req->bRequest) {
    case DFU_REQUEST_DETACH:
    case DFU_XMOS_RESETINTODFU:
    case DFU_XMOS_RESETDEVICE:
        if (stage == CONTROL_STAGE_SETUP) {
            reboot_to = req->bRequest == DFU_XMOS_RESETDEVICE ? 1 : 2;
            return tud_control_status(rhport, req);
        }
        return true;
    case DFU_REQUEST_GETSTATUS:
        return stage == CONTROL_STAGE_SETUP ? get_status(rhport, req) : true;
    case DFU_REQUEST_GETSTATE:
        return stage == CONTROL_STAGE_SETUP ? tud_control_xfer(rhport, req, &state, 1) : true;
    default:
        return false;
    }
}

static bool control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS)
        return false;
    if (stage == CONTROL_STAGE_ACK) {
        if (reboot_to) {
            uint8_t to = reboot_to;
            reboot_to = 0;
            be->reboot(to == 2);
        }
        return true;
    }
    return mode_dfu ? dfu_request(rhport, stage, req) : runtime_request(rhport, stage, req);
}

static bool xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n)
{
    (void)rhport, (void)ep_addr, (void)result, (void)n;
    return false;
}

const usbd_class_driver_t dfu_xmos_driver = {
    .name = "DFU-XMOS",
    .init = init,
    .reset = reset,
    .open = open,
    .control_xfer_cb = control_xfer_cb,
    .xfer_cb = xfer_cb,
};
