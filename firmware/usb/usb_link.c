/* The USB device of a keyboard whose core runs elsewhere (usb_link.h). */
#include <string.h>
#include "usb_link.h"
#include "tusb.h"
#include "class/dfu/dfu.h"
#include "board_config.h"
#include "version.h"

usb_ring_t usb_midi_from_host, usb_midi_to_host, usb_kbd_taps, usb_dev_events;
volatile uint8_t usb_link_mounted;

static uint8_t dfu_mode;
static char serial[25];

int usb_ring_put(usb_ring_t *r, uint32_t v)
{
    uint32_t h = r->head;
    if (h - r->tail >= USB_LINK_RING)
        return 0;
    r->buf[h % USB_LINK_RING] = v;
    r->head = h + 1;
    return 1;
}

static int ring_peek(usb_ring_t *r, uint32_t *v)
{
    if (r->tail == r->head)
        return 0;
    *v = r->buf[r->tail % USB_LINK_RING];
    return 1;
}

int usb_ring_get(usb_ring_t *r, uint32_t *v)
{
    if (!ring_peek(r, v))
        return 0;
    r->tail++;
    return 1;
}

/* ---------- descriptors ---------- */

enum { ITF_MIDI, ITF_MIDI_STREAMING, ITF_HID, ITF_DFU, ITF_TOTAL };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_MIDI, STR_JACK_KEYS, STR_JACK_DAW,
       STR_JACK_MIDI, STR_HID, STR_DFU, STR_COUNT };

#define CABLES 3
#define MIDI_DESC_LEN (TUD_MIDI_DESC_HEAD_LEN + CABLES * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(CABLES))
#define EP_MIDI_OUT 0x01
#define EP_MIDI_IN  0x81
#define EP_HID_IN   0x82
#define DFU_ATTR_RT   0x0F      /* can download, can upload, manifestation tolerant, will detach */
#define DFU_ATTR_MODE 0x07      /* as NI's update mode */
#define DFU_DETACH_MS 250

static const uint8_t desc_hid_report[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = BOARD_USB_VID,
    .idProduct = BOARD_USB_PID,
    .bcdDevice = MK_USB_BCD,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_t desc_device_dfu = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = BOARD_USB_VID,
    .idProduct = BOARD_USB_PID_DFU,
    .bcdDevice = MK_USB_BCD,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + MIDI_DESC_LEN + TUD_HID_DESC_LEN + TUD_DFU_RT_DESC_LEN)

/* self-powered, 0 mA: as NI's configuration descriptor */
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 0),
    TUD_MIDI_DESC_HEAD(ITF_MIDI, STR_MIDI, CABLES),
    TUD_MIDI_DESC_JACK_DESC(1, STR_JACK_KEYS),
    TUD_MIDI_DESC_JACK_DESC(2, STR_JACK_DAW),
    TUD_MIDI_DESC_JACK_DESC(3, STR_JACK_MIDI),
    TUD_MIDI_DESC_EP(EP_MIDI_OUT, 64, CABLES),
    TUD_MIDI_JACKID_IN_EMB(1), TUD_MIDI_JACKID_IN_EMB(2), TUD_MIDI_JACKID_IN_EMB(3),
    TUD_MIDI_DESC_EP(EP_MIDI_IN, 64, CABLES),
    TUD_MIDI_JACKID_OUT_EMB(1), TUD_MIDI_JACKID_OUT_EMB(2), TUD_MIDI_JACKID_OUT_EMB(3),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof desc_hid_report, EP_HID_IN, 8, 5),
    TUD_DFU_RT_DESCRIPTOR(ITF_DFU, STR_DFU, DFU_ATTR_RT, DFU_DETACH_MS, DFU_XMOS_BLOCK),
};

#define CONFIG_DFU_LEN (TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1))

static const uint8_t desc_configuration_dfu[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_DFU_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 0),
    TUD_DFU_DESCRIPTOR(0, 1, STR_DFU, DFU_ATTR_MODE, DFU_DETACH_MS, DFU_XMOS_BLOCK),
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)(dfu_mode ? &desc_device_dfu : &desc_device);
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return dfu_mode ? desc_configuration_dfu : desc_configuration;
}

static const char *const strings[STR_COUNT] = {
    [STR_MANUFACTURER] = "My Kontrol",
    [STR_PRODUCT] = "My Kontrol " BOARD_USB_NAME,
    [STR_MIDI] = "My Kontrol",
    [STR_JACK_KEYS] = "My Kontrol Keys",
    [STR_JACK_DAW] = "My Kontrol DAW",
    [STR_JACK_MIDI] = "My Kontrol MIDI",
    [STR_HID] = "My Kontrol Navigation",
    [STR_DFU] = "My Kontrol Update",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t buf[1 + 32];
    (void)langid;
    int n;
    if (index == STR_LANG) {
        buf[1] = 0x0409;
        n = 1;
    } else {
        if (index >= STR_COUNT)
            return NULL;
        const char *s = index == STR_SERIAL ? serial : strings[index];
        for (n = 0; s[n] && n < 32; n++)
            buf[1 + n] = (uint8_t)s[n];
    }
    buf[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
    return buf;
}

/* ---------- HID: the keyboard ---------- */

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *b, uint16_t len)
{
    (void)instance, (void)id, (void)type, (void)b, (void)len;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, const uint8_t *b, uint16_t len)
{
    (void)instance, (void)id, (void)type, (void)b, (void)len;
}

/* one report per pass: press, then release */
static void kbd_task(void)
{
    static uint8_t key_down;
    uint32_t t;
    if (!tud_hid_ready())
        return;
    if (key_down) {
        tud_hid_keyboard_report(0, 0, NULL);
        key_down = 0;
    } else if (usb_ring_get(&usb_kbd_taps, &t)) {
        uint8_t keys[6] = {(uint8_t)(t >> 8)};
        tud_hid_keyboard_report(0, (uint8_t)t, keys);
        key_down = 1;
    }
}

/* ---------- vendor requests ---------- */

/* 0xB8 OUT, developer builds: an event for the core, wValue = type | id << 8, wIndex =
   value (tools/mktest.py, as on the M32) */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;
#ifdef MK_DEV
    if (req->bmRequestType_bit.direction == TUSB_DIR_OUT && req->bRequest == 0xB8) {
        usb_ring_put(&usb_dev_events, req->wValue | (uint32_t)req->wIndex << 16);
        usb_link_notify();
        return tud_control_status(rhport, req);
    }
#else
    (void)rhport, (void)req;
#endif
    return false;
}

/* ---------- the DFU interface, both modes ---------- */

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
    *count = 1;
    return &dfu_xmos_driver;
}

/* ---------- start and run ---------- */

void tud_mount_cb(void)
{
    usb_link_mounted = 1;
    usb_link_notify();
}

void tud_umount_cb(void)
{
    usb_link_mounted = 0;
    usb_link_notify();
}

void usb_link_init(int dfu, const uint8_t id[12], const dfu_backend_t *backend)
{
    static const char hex[] = "0123456789ABCDEF";
    dfu_mode = (uint8_t)dfu;
    for (int i = 0; i < 12; i++) {
        serial[2 * i] = hex[id[i] >> 4];
        serial[2 * i + 1] = hex[id[i] & 15];
    }
    serial[24] = 0;
    dfu_xmos_setup(backend, dfu);
    tusb_init();
}

void usb_link_task(void)
{
    static uint8_t held;
    static uint32_t held_v;
    uint32_t v;
    uint8_t p[4];
    tud_task();
    if (dfu_mode)
        return;
    int grew = 0;
    for (;;) {                          /* host -> core; what the queue refuses waits */
        if (!held) {
            if (!tud_midi_available() || !tud_midi_packet_read(p))
                break;
            held_v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
            held = 1;
        }
        if (!usb_ring_put(&usb_midi_from_host, held_v))
            break;
        held = 0, grew = 1;
    }
    if (grew)
        usb_link_notify();
    while (ring_peek(&usb_midi_to_host, &v)) {     /* core -> host, dropped without a host */
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
        if (tud_midi_mounted() && !tud_midi_packet_write(p))
            break;
        usb_midi_to_host.tail++;
    }
    kbd_task();
}
