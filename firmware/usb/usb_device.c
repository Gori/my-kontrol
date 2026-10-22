/* USB device (TinyUSB): two MIDI cables plus DFU runtime.
   Cable 1 "Keys" carries music; cable 2 "DAW" carries control (Mackie
   Control or the My Kontrol host protocol), so a DAW never records button
   presses as notes. */
#include "usb.h"
#include "tusb.h"
#include "app.h"
#include "hal.h"
#include "midi.h"
#include "version.h"
#include "kbd.h"

/* platform diagnostics */
extern uint16_t prev_stage, prev_boots, crash_count;
extern uint32_t reset_flags;
int boot_flag_armed(void);

static volatile int detach_requested;

enum { ITF_NUM_MIDI, ITF_NUM_MIDI_STREAMING, ITF_NUM_DFU_RT, ITF_NUM_HID, ITF_NUM_TOTAL };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_DFU_RT, STR_MIDI, STR_JACK_KEYS, STR_JACK_DAW, STR_SERIAL, STR_HID, STR_COUNT };

#define MIDI_DESC_LEN (TUD_MIDI_DESC_HEAD_LEN + 2 * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(2))
#define EP_MIDI_OUT 0x01
#define EP_MIDI_IN  0x81
#define EP_HID_IN   0x82

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

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + MIDI_DESC_LEN + TUD_DFU_RT_DESC_LEN + TUD_HID_DESC_LEN)

/* DFU functional values as in NI's descriptor: canDnload | willDetach,
   255 ms detach timeout, 64-byte transfers. */
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 480),
    TUD_MIDI_DESC_HEAD(ITF_NUM_MIDI, STR_MIDI, 2),
    TUD_MIDI_DESC_JACK_DESC(1, STR_JACK_KEYS),
    TUD_MIDI_DESC_JACK_DESC(2, STR_JACK_DAW),
    TUD_MIDI_DESC_EP(EP_MIDI_OUT, 64, 2), TUD_MIDI_JACKID_IN_EMB(1), TUD_MIDI_JACKID_IN_EMB(2),
    TUD_MIDI_DESC_EP(EP_MIDI_IN, 64, 2), TUD_MIDI_JACKID_OUT_EMB(1), TUD_MIDI_JACKID_OUT_EMB(2),
    TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STR_DFU_RT, 0x09, 255, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof desc_hid_report, EP_HID_IN, 8, 5),
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static int build_string(uint8_t index, char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    char *p = s;
    const char *t = 0;
    switch (index) {
    case STR_MANUFACTURER: t = "My Kontrol"; break;
    case STR_MIDI:         t = "My Kontrol"; break;
    case STR_JACK_KEYS:    t = "My Kontrol Keys"; break;
    case STR_JACK_DAW:     t = "My Kontrol DAW"; break;
    case STR_DFU_RT:       t = "My Kontrol Update"; break;
    case STR_HID:          t = "My Kontrol Navigation"; break;
    case STR_PRODUCT:
        for (t = "My Kontrol "; *t; ) *p++ = *t++;
        t = board_info.short_name;
        break;
    case STR_SERIAL: {
        int n = board_serial(s, 33);
        if (n > 0)
            return n;
        uint8_t id[12];
        hal_unique_id(id);
        for (int i = 0; i < 12; i++) {
            *p++ = hex[id[i] >> 4];
            *p++ = hex[id[i] & 15];
        }
        break;
    }
    default:
        return -1;
    }
    while (t && *t)
        *p++ = *t++;
    *p = 0;
    return p - s;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t buf[1 + 32];
    char s[33];
    (void)langid;
    int n;
    if (index == STR_LANG) {
        buf[1] = 0x0409;
        n = 1;
    } else {
        if ((n = build_string(index, s)) < 0)
            return NULL;
        if (n > 32)
            n = 32;
        for (int i = 0; i < n; i++)
            buf[1 + i] = s[i];
    }
    buf[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return buf;
}

/* Vendor requests:
   0xB3 IN   NI's flash-type query; "none" (tools/mkflash.py and the web
             installer read it before detaching, as with NI's firmware)
   0xB5 IN   diagnostics (tools/mkdiag.py)
   0xB8 OUT  developer builds only: inject an event, wValue = type | id << 8,
             wIndex = value (tools/mktest.py)
   0xBD IN   developer builds only: read memory (tools/mkpeek.py) */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    static const uint8_t flash_type[4] = {'n', 'o', 'n', 'e'};
    static uint8_t diag[64];
    int in = req->bmRequestType_bit.direction == TUSB_DIR_IN;

    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (in && req->bRequest == 0xB3)
        return tud_control_xfer(rhport, req, (void *)(uintptr_t)flash_type, sizeof flash_type);
    if (in && req->bRequest == 0xB5) {
        int n = 0;
        diag[n++] = 2;                     /* layout version */
        diag[n++] = boot_flag_armed();
        diag[n++] = reset_flags;
        diag[n++] = reset_flags >> 8;
        diag[n++] = reset_flags >> 16;
        diag[n++] = reset_flags >> 24;
        diag[n++] = prev_stage;
        diag[n++] = prev_stage >> 8;
        diag[n++] = prev_boots;
        diag[n++] = prev_boots >> 8;
        diag[n++] = crash_count;
        diag[n++] = midi_tx_dropped;
        diag[n++] = midi_tx_dropped >> 8;
        diag[n++] = app_link();
        n += board_diag(diag + n, sizeof diag - n);
        return tud_control_xfer(rhport, req, diag, n);
    }
#ifdef MK_DEV
    /* 0xBD IN: read memory, address = wIndex << 16 | wValue (porting aid) */
    if (in && req->bRequest == 0xBD) {
        uintptr_t a = (uintptr_t)req->wIndex << 16 | req->wValue;
        uint16_t n = req->wLength > sizeof diag ? sizeof diag : req->wLength;
        for (int i = 0; i < n; i++)
            diag[i] = *(const volatile uint8_t *)(a + i);
        return tud_control_xfer(rhport, req, diag, n);
    }
    if (!in && req->bRequest == 0xB8) {
        mk_event_t ev = {req->wValue & 0xFF, req->wValue >> 8, (int16_t)req->wIndex};
        app_event(&ev);
        return tud_control_status(rhport, req);
    }
#endif
#ifdef RECOVERY_TESTS
    /* 0xB6: hang (watchdog path), 0xB7: fault (fault-handler path) */
    if (req->bRequest == 0xB6)
        for (;;) ;
    if (req->bRequest == 0xB7)
        ((void (*)(void))0xFFFFFFF1)();
#endif
    return false;
}

/* ---------- keyboard taps (core/kbd.h) ---------- */

#define TAPS 32
static uint8_t tap_mod[TAPS], tap_key[TAPS], tap_head, tap_tail, key_down;

void kbd_tap(uint8_t modifier, uint8_t key)
{
    uint8_t next = (tap_head + 1) % TAPS;
    if (next == tap_tail || !tud_mounted())
        return;
    tap_mod[tap_head] = modifier;
    tap_key[tap_head] = key;
    tap_head = next;
}

/* one report per poll: press, then release */
static void kbd_task(void)
{
    if (!tud_hid_ready())
        return;
    if (key_down) {
        tud_hid_keyboard_report(0, 0, NULL);
        key_down = 0;
    } else if (tap_tail != tap_head) {
        uint8_t keys[6] = {tap_key[tap_tail]};
        tud_hid_keyboard_report(0, tap_mod[tap_tail], keys);
        tap_tail = (tap_tail + 1) % TAPS;
        key_down = 1;
    }
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t len)
{
    (void)instance; (void)id; (void)type; (void)buf; (void)len;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, const uint8_t *buf, uint16_t len)
{
    (void)instance; (void)id; (void)type; (void)buf; (void)len;
}

/* Called from tud_task before the status stage has gone out; usb_task reboots. */
void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
    detach_requested = 1;
}

void tud_mount_cb(void) { app_usb_mounted(1); }
void tud_umount_cb(void) { app_usb_mounted(0); }

void dcd_connect(uint8_t rhport)
{
    (void)rhport;
    board_usb_pullup(1);
}

void dcd_disconnect(uint8_t rhport)
{
    (void)rhport;
    board_usb_pullup(0);
}

void usb_irq(void)
{
    tud_int_handler(0);
}

void usb_init(void)
{
    hal_usb_init();
    tusb_init();                       /* also raises the pull-up via dcd_connect */
    __asm volatile("cpsie i");
}

void usb_task(void)
{
    uint8_t pkt[4];
    tud_task();
    while (tud_midi_available() && tud_midi_packet_read(pkt))
        midi_rx_packet(pkt);
    if (tud_midi_mounted()) {
        while (midi_tx_peek(pkt) && tud_midi_packet_write(pkt))
            midi_tx_pop();
    } else {
        while (midi_tx_peek(pkt))
            midi_tx_pop();
    }
    kbd_task();
    if (detach_requested) {
        hal_delay_ms(10);              /* let the ISR send the DETACH status stage */
        app_enter_bootloader();
    }
}
