/* Host test of the S MK1's USB device (usb/usb_link.c, usb/dfu_xmos.c, tile 1's DFU backend
   platform/xs1/tile1_dfu.c with upgrade.c on a fake SPI flash): TinyUSB as built for tile 1,
   with a fake DCD that plays the host the way the SIE thread hands transactions to TinyUSB
   (setup, data stages, status, SET_ADDRESS answered by the DCD).

   What it shows: the descriptors parse and are what the host sees (both devices), the MIDI
   cables and the keyboard move data both ways through the queues, RESETINTODFU / DETACH /
   RESETFROMDFU reboot only after their status stage, and DFU mode downloads an image to the
   upgrade slot (and nowhere else), reads it back, refuses bad images, and keeps to DFU 1.1's
   state machine. What it can't show: anything on the wire (usb_ulpi.c, the PHY). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tusb.h"
#include "device/dcd.h"
#include "class/dfu/dfu.h"
#include "usb_link.h"
#include "tile1_dfu.h"
#include "upgrade.h"
#include "fake_flash.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- the fake DCD and host ---------- */

typedef struct { uint8_t *buf; uint16_t len; int pending; } xfer_t;
static xfer_t xf[8][2];                     /* [endpoint number][0 OUT, 1 IN] */
static int ep0_stalled, in_status, notified;
static uint8_t dev_addr;
static int reboots, reboot_dfu, reboot_early;

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) { (void)rhport, (void)rh_init; return true; }
void dcd_int_enable(uint8_t rhport) { (void)rhport; }
void dcd_int_disable(uint8_t rhport) { (void)rhport; }
void dcd_int_handler(uint8_t rhport) { (void)rhport; }
void dcd_remote_wakeup(uint8_t rhport) { (void)rhport; }
void dcd_sof_enable(uint8_t rhport, bool en) { (void)rhport, (void)en; }
void dcd_edpt_close_all(uint8_t rhport) { (void)rhport; }
bool dcd_edpt_iso_alloc(uint8_t r, uint8_t e, uint16_t s) { (void)r, (void)e, (void)s; return false; }
bool dcd_edpt_iso_activate(uint8_t r, tusb_desc_endpoint_t const *d) { (void)r, (void)d; return false; }
bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *d) { (void)rhport, (void)d; return true; }
void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep) { (void)rhport, (void)ep; }

void dcd_set_address(uint8_t rhport, uint8_t a)
{
    (void)rhport;
    dev_addr = a;
    xf[0][1] = (xfer_t){NULL, 0, 1};        /* the status ZLP, sent by the DCD (as the SIE does) */
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *buf, uint16_t len, bool isr)
{
    (void)rhport, (void)isr;
    xf[ep & 7][ep >> 7] = (xfer_t){buf, len, 1};
    return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep)
{
    (void)rhport;
    if ((ep & 7) == 0)
        ep0_stalled = 1;
}

void usb_link_notify(void) { notified++; }
uint32_t tusb_time_millis_api(void) { return 0; }

static void fake_reboot(int into_dfu)
{
    reboots++;
    reboot_dfu = into_dfu;
    if (!in_status)
        reboot_early++;
}

static int dfu_mode;
static void run(void)
{
    if (dfu_mode)
        tud_task();
    else
        usb_link_task();
}

static void complete(uint8_t ep, uint32_t n)
{
    xf[ep & 7][ep >> 7].pending = 0;
    dcd_event_xfer_complete(0, ep, n, XFER_RESULT_SUCCESS, false);
    run();
}

/* one control transfer: bytes of the data stage, -1 = stalled, -2 = the device did not answer */
static int control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, uint8_t *data)
{
    uint8_t s[8] = {type, req, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
                    (uint8_t)len, (uint8_t)(len >> 8)};
    memset(xf[0], 0, sizeof xf[0]);
    ep0_stalled = 0;
    dcd_event_setup_received(0, s, false);
    run();
    int n = 0, in = type & 0x80;
    while (n < len) {
        if (ep0_stalled)
            return -1;
        xfer_t *x = &xf[0][in ? 1 : 0];
        if (!x->pending)
            return -2;
        uint16_t k = x->len;
        if (in) {
            memcpy(data + n, x->buf, k);
        } else {
            if (k > len - n)
                k = (uint16_t)(len - n);
            memcpy(x->buf, data + n, k);
        }
        n += k;
        complete(in ? 0x80 : 0x00, k);
        if (in && k < CFG_TUD_ENDPOINT0_SIZE)
            break;
    }
    if (ep0_stalled)
        return -1;
    xfer_t *st = &xf[0][in && len ? 0 : 1];
    if (!st->pending)
        return -2;
    in_status = 1;
    complete(in && len ? 0x00 : 0x80, 0);
    in_status = 0;
    return n;
}

static int bulk_in(uint8_t ep, uint8_t *out)
{
    xfer_t *x = &xf[ep & 7][1];
    if (!x->pending)
        return -1;
    memcpy(out, x->buf, x->len);
    int n = x->len;
    complete(ep, n);
    return n;
}

static int bulk_out(uint8_t ep, const uint8_t *p, uint16_t n)
{
    xfer_t *x = &xf[ep & 7][0];
    if (!x->pending || x->len < n)
        return -1;
    memcpy(x->buf, p, n);
    complete(ep, n);
    return n;
}

#define GET_DESC(t, i, len, buf) control(0x80, 6, (uint16_t)((t) << 8 | (i)), (t) == 3 ? 0x0409 : 0, len, buf)

static void string_of(uint8_t index, char *out)
{
    uint8_t b[66];
    int n = GET_DESC(3, index, sizeof b, b);
    int k = 0;
    for (int i = 2; i + 1 < n && i < b[0]; i += 2)
        out[k++] = (char)b[i];
    out[k] = 0;
}

static void start(int dfu, uint8_t *dev, uint8_t *cfg, int *cfg_len)
{
    static const uint8_t id[12] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8};
    if (tud_inited())
        tusb_deinit(0);
    memset(xf, 0, sizeof xf);
    dfu_mode = dfu;
    reboots = reboot_early = 0;
    usb_link_init(dfu, id, tile1_dfu_backend(&fake_flash_bus, SPI_FLASH_AT25DF081A, fake_reboot));
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    run();
    CHECK(GET_DESC(1, 0, 64, dev) == 18, "device descriptor");
    CHECK(control(0x00, 5, 7, 0, 0, NULL) == 0 && dev_addr == 7, "SET_ADDRESS");
    uint8_t head[9];
    CHECK(GET_DESC(2, 0, 9, head) == 9, "configuration header");
    *cfg_len = head[2] | head[3] << 8;
    CHECK(GET_DESC(2, 0, (uint16_t)*cfg_len, cfg) == *cfg_len, "configuration, %d bytes", *cfg_len);
    CHECK(control(0x00, 9, 1, 0, 0, NULL) == 0, "SET_CONFIGURATION");
    run();
}

/* ---------- the normal device ---------- */

static void test_app(void)
{
    uint8_t dev[18], cfg[512];
    int len;
    start(0, dev, cfg, &len);
    CHECK(dev[0] == 18 && dev[1] == 1 && (dev[2] | dev[3] << 8) == 0x0200, "device descriptor header");
    CHECK((dev[8] | dev[9] << 8) == 0x17CC && (dev[10] | dev[11] << 8) == 0x1350, "VID/PID 17cc:1350");
    CHECK(dev[7] == 64 && dev[17] == 1, "EP0 64, one configuration");
    CHECK(tud_mounted() && usb_link_mounted && notified, "mounted, the link side told");

    /* walk the configuration */
    int itf[8][3], nitf = 0, eps = 0, jacks_in = 0, jacks_out = 0, ok_len = 1, cables[2] = {0, 0};
    uint8_t ep_addr[8], ep_attr[8];
    uint16_t ep_size[8];
    uint8_t dfu_attr = 0, dfu_xfer = 0, iface_str[8] = {0};
    for (int i = 0; i < len;) {
        uint8_t l = cfg[i], t = cfg[i + 1];
        if (l < 2 || i + l > len) {
            ok_len = 0;
            break;
        }
        if (t == 4 && nitf < 8) {
            itf[nitf][0] = cfg[i + 5], itf[nitf][1] = cfg[i + 6], itf[nitf][2] = cfg[i + 7];
            iface_str[nitf] = cfg[i + 8];
            CHECK(cfg[i + 2] == nitf, "interface %d numbered %d", nitf, cfg[i + 2]);
            nitf++;
        } else if (t == 5 && eps < 8) {
            ep_addr[eps] = cfg[i + 2], ep_attr[eps] = cfg[i + 3], ep_size[eps] = (uint16_t)(cfg[i + 4] | cfg[i + 5] << 8);
            eps++;
        } else if (t == 0x24 && cfg[i + 2] == 2) {
            jacks_in++;
        } else if (t == 0x24 && cfg[i + 2] == 3) {
            jacks_out++;
        } else if (t == 0x25 && cfg[i + 2] == 1 && eps <= 2) {
            cables[eps - 1] = cfg[i + 3];
        } else if (t == 0x21 && l == 9 && nitf == 4) {          /* after the DFU interface (HID uses 0x21 too) */
            dfu_attr = cfg[i + 2], dfu_xfer = cfg[i + 5];
            CHECK((cfg[i + 7] | cfg[i + 8] << 8) == 0x0110, "DFU 1.1");
        }
        i += l;
    }
    CHECK(ok_len && cfg[4] == 4 && nitf == 4, "4 interfaces (%d), lengths consistent", nitf);
    CHECK(itf[0][0] == 1 && itf[0][1] == 1 && itf[1][0] == 1 && itf[1][1] == 3, "0 audio control, 1 MIDI streaming");
    CHECK(itf[2][0] == 3, "2 HID");
    CHECK(itf[3][0] == 0xFE && itf[3][1] == 1 && itf[3][2] == 1, "3 DFU runtime (NI's updater sends RESETINTODFU to 3)");
    CHECK(eps == 3 && ep_addr[0] == 0x01 && ep_addr[1] == 0x81 && ep_addr[2] == 0x82, "endpoints 01 81 82");
    CHECK((ep_attr[0] & 3) == 2 && (ep_attr[1] & 3) == 2 && (ep_attr[2] & 3) == 3, "bulk, bulk, interrupt");
    CHECK(ep_size[0] == 64 && ep_size[1] == 64 && ep_size[2] == 8, "full-speed packet sizes");
    CHECK(jacks_in == 6 && jacks_out == 6 && cables[0] == 3 && cables[1] == 3, "3 MIDI cables (%d in, %d out jacks, %d/%d)",
          jacks_in, jacks_out, cables[0], cables[1]);
    CHECK(dfu_attr == 0x0F && dfu_xfer == 64, "DFU attributes 0x%02x, transfer %d", dfu_attr, dfu_xfer);
    CHECK((cfg[7] & 0x40) && cfg[8] == 0, "self-powered, 0 mA (as NI's)");

    char s[40];
    string_of(dev[14], s);
    CHECK(!strcmp(s, "My Kontrol"), "manufacturer '%s'", s);
    string_of(dev[15], s);
    CHECK(!strcmp(s, "My Kontrol S49"), "product '%s'", s);
    string_of(dev[16], s);
    CHECK(!strcmp(s, "DEADBEEF0102030405060708"), "serial '%s'", s);
    string_of(iface_str[3], s);
    CHECK(!strcmp(s, "My Kontrol Update"), "DFU interface '%s'", s);
    const char *jack[3] = {"My Kontrol Keys", "My Kontrol DAW", "My Kontrol MIDI"};
    for (int c = 0; c < 3; c++) {
        string_of((uint8_t)(5 + c), s);
        CHECK(!strcmp(s, jack[c]), "cable %d '%s'", c, s);
    }

    /* MIDI both ways, cable numbers kept */
    const uint8_t from_host[8] = {0x09, 0x90, 0x3C, 0x40, 0x2B, 0xB0, 0x07, 0x64};
    CHECK(bulk_out(0x01, from_host, 8) == 8, "host sends 2 packets");
    run();
    uint32_t v = 0, w = 0;
    CHECK(usb_ring_get(&usb_midi_from_host, &v) && v == 0x403C9009u && usb_ring_get(&usb_midi_from_host, &w) &&
          w == 0x6407B02Bu, "to the link: cable 0 note on, cable 2 CC (%08x %08x)", v, w);
    usb_ring_put(&usb_midi_to_host, 0x7F3C9009u);
    usb_ring_put(&usb_midi_to_host, 0x000040BB1u);
    run();
    uint8_t in[128];
    int n = 0, k;
    for (int i = 0; i < 4 && (k = bulk_in(0x81, in + n)) > 0; i++) {
        n += k;
        run();
    }
    CHECK(n == 8 && in[0] == 0x09 && in[2] == 0x3C && in[4] == 0xB1, "to the host: both packets (%d bytes)", n);

    /* the keyboard: a tap = press, release */
    usb_ring_put(&usb_kbd_taps, 0x02 | 0x52 << 8);
    run();
    n = bulk_in(0x82, in);
    CHECK(n == 8 && in[0] == 0x02 && in[2] == 0x52, "key press: shift + up");
    run();
    n = bulk_in(0x82, in);
    CHECK(n == 8 && in[0] == 0 && in[2] == 0, "key release");

    /* DFU runtime */
    uint8_t st[6];
    CHECK(control(0xA1, DFU_REQUEST_GETSTATUS, 0, 3, 6, st) == 6 && st[4] == 0, "GETSTATUS: appIDLE");
    CHECK(control(0x21, 0x55, 0, 3, 0, NULL) == -1, "an unknown request is stalled");
    CHECK(control(0x21, DFU_XMOS_RESETINTODFU, 0, 3, 0, NULL) == 0 && reboots == 1 && reboot_dfu == 1 &&
          !reboot_early, "RESETINTODFU: reboot into DFU after the status stage");
    CHECK(control(0x21, DFU_REQUEST_DETACH, 250, 3, 0, NULL) == 0 && reboots == 2 && reboot_dfu == 1 && !reboot_early,
          "DETACH: the same");
    CHECK(control(0x21, DFU_XMOS_REVERTFACTORY, 0, 3, 0, NULL) == -1, "REVERTFACTORY only in DFU mode");
}

/* ---------- DFU mode ---------- */

static uint8_t img[0x40000], back[0x40000], snap[SPI_FLASH_SIZE];

static int getstatus(uint8_t *state)
{
    uint8_t st[6];
    if (control(0xA1, DFU_REQUEST_GETSTATUS, 0, 0, 6, st) != 6)
        return -1;
    *state = st[4];
    return st[0];
}

/* the updater's download: 64-byte blocks, GETSTATUS after each, a zero-length block, GETSTATUS */
static int dnload(const uint8_t *p, uint32_t n, uint8_t *state)
{
    uint16_t block = 0;
    for (uint32_t o = 0; o < n; o += 64, block++) {
        uint16_t k = (uint16_t)(n - o < 64 ? n - o : 64);
        if (control(0x21, DFU_REQUEST_DNLOAD, block, 0, k, (uint8_t *)p + o) != k)
            return -100;
        int s = getstatus(state);
        if (s)
            return s;
        if (*state != DFU_DNLOAD_IDLE)
            return -101;
    }
    if (control(0x21, DFU_REQUEST_DNLOAD, block, 0, 0, NULL) != 0)
        return -102;
    return getstatus(state);
}

static void test_dfu(void)
{
    uint8_t dev[18], cfg[64], state = 0;
    int len;
    fake_flash_layout(1, 0x16000);
    memcpy(snap, fake_flash.mem, sizeof snap);
    start(1, dev, cfg, &len);
    CHECK((dev[10] | dev[11] << 8) == 0x1352, "DFU mode PID 0x1352");
    CHECK(len == 9 + 9 + 9 && cfg[4] == 1, "one interface (%d bytes)", len);
    CHECK(cfg[9 + 5] == 0xFE && cfg[9 + 6] == 1 && cfg[9 + 7] == 2, "DFU mode interface FE/01/02");
    CHECK(cfg[18 + 2] == 0x07 && cfg[18 + 5] == 64, "attributes 0x07 (as NI's), transfer 64");
    CHECK(getstatus(&state) == 0 && state == DFU_IDLE, "dfuIDLE");

    /* out-of-order requests */
    CHECK(control(0x21, DFU_REQUEST_DNLOAD, 0, 0, 0, NULL) == -1, "a zero-length DNLOAD in dfuIDLE is stalled");
    CHECK(control(0x21, DFU_REQUEST_DETACH, 0, 0, 0, NULL) == -1, "DETACH in DFU mode is stalled");

    /* a good image */
    uint32_t size = fake_image(img, 0xC000, 0x8000, 0x77, 1);
    CHECK(dnload(img, size, &state) == 0 && state == DFU_IDLE, "download of %u bytes, manifest, back to dfuIDLE", size);
    upgrade_slot_t s;
    CHECK(upgrade_scan(&fake_flash_bus, &s) == 0 && s.image == 0x16000 && s.len, "the image is in the slot");
    CHECK(!memcmp(fake_flash.mem + 0x16000, img, size), "byte for byte");
    CHECK(!memcmp(fake_flash.mem, snap, 0x16000) && !memcmp(fake_flash.mem + 0x36000, snap + 0x36000, SPI_FLASH_SIZE - 0x36000),
          "boot loader, factory image, NI's data and settings untouched");

    /* upload */
    uint32_t n = 0;
    for (uint16_t b = 0;; b++) {
        int k = control(0xA1, DFU_REQUEST_UPLOAD, b, 0, 64, back + n);
        if (k < 0)
            break;
        n += (uint32_t)k;
        if (k < 64)
            break;
    }
    CHECK(n == size && !memcmp(back, img, size), "upload gives it back (%u of %u bytes)", n, size);
    CHECK(getstatus(&state) == 0 && state == DFU_IDLE, "dfuIDLE after the short block");

    /* a corrupted image: refused at the manifest, state error until CLRSTATUS */
    img[0x9000] ^= 4;
    CHECK(dnload(img, size, &state) == DFU_STATUS_ERR_FILE && state == DFU_ERROR, "a bad image: errFILE, dfuERROR");
    CHECK(upgrade_scan(&fake_flash_bus, &s) == 0 && s.len == 0, "no valid upgrade image left (the factory one boots)");
    CHECK(control(0x21, DFU_REQUEST_DNLOAD, 0, 0, 64, img) == -1, "no DNLOAD in dfuERROR");
    CHECK(control(0x21, DFU_REQUEST_CLRSTATUS, 0, 0, 0, NULL) == 0 && getstatus(&state) == 0 && state == DFU_IDLE,
          "CLRSTATUS");
    img[0x9000] ^= 4;

    /* cut off: ABORT after half */
    for (uint16_t b = 0; b < 200; b++) {
        control(0x21, DFU_REQUEST_DNLOAD, b, 0, 64, img + 64 * b);
        getstatus(&state);
    }
    CHECK(control(0x21, DFU_REQUEST_ABORT, 0, 0, 0, NULL) == 0 && getstatus(&state) == 0 && state == DFU_IDLE, "ABORT");
    CHECK(upgrade_scan(&fake_flash_bus, &s) == 0 && s.len == 0, "an aborted download leaves no valid image");

    /* too big */
    uint32_t big = fake_image(img, 0x14000, 0x10000, 0x33, 1);
    CHECK(dnload(img, big, &state) == DFU_STATUS_ERR_ADDRESS && state == DFU_ERROR, "an image bigger than the slot: errADDRESS");
    control(0x21, DFU_REQUEST_CLRSTATUS, 0, 0, 0, NULL);

    /* the good one again, then REVERTFACTORY */
    size = fake_image(img, 0xC000, 0x8000, 0x77, 1);
    CHECK(dnload(img, size, &state) == 0, "download again");
    CHECK(control(0x21, DFU_XMOS_REVERTFACTORY, 0, 0, 0, NULL) == 0 && getstatus(&state) == 0,
          "REVERTFACTORY");
    CHECK(upgrade_scan(&fake_flash_bus, &s) == 0 && s.len == 0, "no upgrade image: the factory image boots");
    CHECK(control(0x21, DFU_XMOS_SELECTIMAGE, 0, 0, 0, NULL) == 0, "SELECTIMAGE accepted (does nothing)");
    CHECK(control(0x21, DFU_XMOS_RESETFROMDFU, 0, 0, 0, NULL) == 0 && reboots == 1 && reboot_dfu == 0 && !reboot_early,
          "RESETFROMDFU: reboot into the normal device after the status stage");

    /* a flash without the factory image we expect: nothing is written */
    memset(fake_flash.mem, 0xFF, sizeof fake_flash.mem);
    fake_flash.changes = 0;
    CHECK(control(0x21, DFU_REQUEST_DNLOAD, 0, 0, 64, img) == 64 && getstatus(&state) == DFU_STATUS_ERR_TARGET &&
          state == DFU_ERROR && fake_flash.changes == 0, "unknown flash layout: errTARGET, %d changes", fake_flash.changes);
}

int main(void)
{
#define RUN(t) do { current = #t; t; } while (0)
    RUN(test_app());
    RUN(test_dfu());
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("test_usbdev: all passed\n");
    return 0;
}
