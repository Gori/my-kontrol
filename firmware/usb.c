#include "hw.h"
#include "tusb.h"
#include "app.h"
#include "ui.h"
#include "strips.h"
#include "knobs.h"
#include "pedal.h"

volatile int usb_configured;
static volatile int detach_requested;

enum { ITF_NUM_MIDI, ITF_NUM_MIDI_STREAMING, ITF_NUM_DFU_RT, ITF_NUM_TOTAL };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_DFU_RT, STR_MIDI, STR_JACK_KEYS, STR_JACK_DAW, STR_SERIAL };

/* Two MIDI cables: 1 = keys/Smart Play, 2 = DAW control (Mackie). Keeping
   them apart stops the DAW from playing MCU button notes as music. */
#define MIDI_DESC_LEN (TUD_MIDI_DESC_HEAD_LEN + 2 * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(2))

#define EP_MIDI_OUT 0x01
#define EP_MIDI_IN  0x81

/* NI's VID/PID so m32dfu.py and NI's updater still find the device. bcdDevice
   differs from NI's 0x0045 so hosts don't reuse cached NI descriptors. */
static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x17CC,
    .idProduct = 0x1860,
    .bcdDevice = 0x0200,
    .iSerialNumber = STR_SERIAL,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + MIDI_DESC_LEN + TUD_DFU_RT_DESC_LEN)

/* DFU functional values copied from NI's app descriptor: canDnload | willDetach,
   255 ms detach timeout, 64-byte transfers. 480 mA is what NI requests too. */
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 480),
    TUD_MIDI_DESC_HEAD(ITF_NUM_MIDI, STR_MIDI, 2),
    TUD_MIDI_DESC_JACK_DESC(1, STR_JACK_KEYS),
    TUD_MIDI_DESC_JACK_DESC(2, STR_JACK_DAW),
    TUD_MIDI_DESC_EP(EP_MIDI_OUT, 64, 2), TUD_MIDI_JACKID_IN_EMB(1), TUD_MIDI_JACKID_IN_EMB(2),
    TUD_MIDI_DESC_EP(EP_MIDI_IN, 64, 2), TUD_MIDI_JACKID_OUT_EMB(1), TUD_MIDI_JACKID_OUT_EMB(2),
    TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STR_DFU_RT, 0x09, 255, 64),
};

static const char *const strings[] = {
    [STR_MANUFACTURER] = "Native Instruments",
    [STR_PRODUCT] = "M32 (Oskar's Firmware)",
    [STR_DFU_RT] = "DFU runtime",
    [STR_MIDI] = "M32",
    [STR_JACK_KEYS] = "M32 Keys",
    [STR_JACK_DAW] = "M32 DAW",
    [STR_SERIAL] = "OSK-M32-2",
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

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t buf[1 + 32];
    (void)langid;

    uint8_t n;
    if (index == STR_LANG) {
        buf[1] = 0x0409;
        n = 1;
    } else if (index < TU_ARRAY_SIZE(strings) && strings[index]) {
        const char *s = strings[index];
        for (n = 0; s[n] && n < 32; n++)
            buf[1 + n] = s[n];
    } else {
        return NULL;
    }
    buf[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return buf;
}

/* m32dfu.py `enter` reads NI's flash-type request (0xB3) before detaching and
   aborts on a stall. "none" matches the tool's --expect-type default. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    static const uint8_t flash_type[4] = {'n', 'o', 'n', 'e'};

    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (req->bmRequestType_bit.direction == TUSB_DIR_IN && req->bRequest == 0xB3)
        return tud_control_xfer(rhport, req, (void *)(uintptr_t)flash_type, sizeof flash_type);
    /* Diagnostics: boot flag state, mount state, RCC_CSR reset cause */
    if (req->bmRequestType_bit.direction == TUSB_DIR_IN && req->bRequest == 0xB5) {
        static uint8_t diag[10 + 32 + 1 + 6];
        uint32_t csr = reset_flags;
        diag[0] = boot_flag_armed();
        diag[1] = usb_configured;
        diag[2] = csr;
        diag[3] = csr >> 8;
        diag[4] = csr >> 16;
        diag[5] = csr >> 24;
        diag[6] = prev_stage;
        diag[7] = prev_stage >> 8;
        diag[8] = prev_boots;
        diag[9] = prev_boots >> 8;
        for (int i = 0; i < 32; i++)
            diag[10 + i] = flag_page_snapshot[i];
        diag[42] = hw_status;
        for (int i = 0; i < 6; i++)
            diag[43 + i] = strips_probe_raw[i / 3][i % 3];
        return tud_control_xfer(rhport, req, diag, sizeof diag);
    }
    if (req->bmRequestType_bit.direction == TUSB_DIR_IN && req->bRequest == 0xBA)
        return tud_control_xfer(rhport, req, knobs_raw, sizeof knobs_raw);
    if (req->bmRequestType_bit.direction == TUSB_DIR_IN && req->bRequest == 0xBB)
        return tud_control_xfer(rhport, req, pedal_raw, sizeof pedal_raw);
    /* Test hooks: 0xB8 UI event (wValue = type, wIndex = signed value),
       0xB9 key (wValue = note | on << 8, wIndex = velocity) */
    if (req->bmRequestType_bit.direction == TUSB_DIR_OUT && req->bRequest == 0xB8) {
        ui_event(req->wValue, 0, (int16_t)req->wIndex);
        return tud_control_status(rhport, req);
    }
    if (req->bmRequestType_bit.direction == TUSB_DIR_OUT && req->bRequest == 0xB9) {
        app_key(req->wValue & 0x7F, req->wIndex, req->wValue >> 8);
        return tud_control_status(rhport, req);
    }
#ifdef RECOVERY_TESTS
    /* 0xB6: hang (watchdog path), 0xB7: fault (fault-handler path) */
    if (req->bRequest == 0xB6)
        for (;;) ;
    if (req->bRequest == 0xB7)
        ((void (*)(void))0xFFFFFFF1)();
#endif
    return false;
}

/* Called from tud_task before the status stage has gone out; usb_poll reboots. */
void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
    detach_requested = 1;
}

void tud_mount_cb(void)
{
    usb_configured = 1;
}

void tud_umount_cb(void)
{
    usb_configured = 0;
}

/* The F1 has no internal D+ pull-up; the board switches one with PA8. */
void dcd_connect(uint8_t rhport)
{
    (void)rhport;
    GPIOA_BSRR = 1 << 8;
}

void dcd_disconnect(uint8_t rhport)
{
    (void)rhport;
    GPIOA_BRR = 1 << 8;
}

void usb_irq(void)
{
    tud_int_handler(0);
}

void usb_init(void)
{
    RCC_APB1ENR |= 1 << 23;            /* USB clock; USBPRE was set in clock_init */
    RCC_APB1RSTR |= 1 << 23;           /* drop whatever state the bootloader left */
    RCC_APB1RSTR &= ~(1u << 23);

    tusb_init();                       /* also raises the pull-up via dcd_connect */
    __asm volatile("cpsie i");
}

void usb_poll(void)
{
    tud_task();
#ifdef MIDI_LOOPBACK
    uint8_t pkt[4];
    while (tud_midi_available() && tud_midi_packet_read(pkt))
        tud_midi_packet_write(pkt);
#endif
    if (detach_requested) {
        delay_ms(10);                  /* let the ISR send the DETACH status stage */
        enter_bootloader();
    }
}
