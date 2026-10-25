/* Host tests for the XS1 ULPI USB driver's pure logic (firmware/platform/xs1):
   CRCs, packet framing, the ULPI register-cycle state machine against a fake
   ULPI bus, and the DCD endpoint/control state machine through a simulated
   enumeration (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION).
   No XS1 ports here: usb_ulpi.c / dcd_xs1_ulpi.c are excluded from this build. */
#include <stdio.h>
#include <string.h>
#include "usb_crc.h"
#include "usb_packet.h"
#include "usb_ep.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- CRCs and framing ---------------- */

static void test_crc_and_framing(void)
{
    current = "crc5";
    /* Hand-computed: CRC5 over 11 zero bits = 0x02 (USB 2.0 §8.3.5.1). */
    CHECK(usb_crc5(0, 11) == 0x02, "got 0x%02x", usb_crc5(0, 11));

    current = "crc16_empty";
    CHECK(usb_crc16(NULL, 0) == 0x0000, "got 0x%04x", usb_crc16(NULL, 0));

    current = "pid";
    CHECK(usb_pid_byte(USB_PID_SETUP) == 0x2D, "got 0x%02x", usb_pid_byte(USB_PID_SETUP));
    CHECK(usb_pid_byte(USB_PID_IN)    == 0x69, "got 0x%02x", usb_pid_byte(USB_PID_IN));
    CHECK(usb_pid_valid(0x2D) && !usb_pid_valid(0x2C), "pid validity");

    current = "token_setup_0_0";
    uint8_t tok[3];
    usb_build_token(tok, USB_PID_SETUP, 0, 0);
    /* field=0, crc5=0x02 -> full=0x1000 -> {0x2D,0x00,0x10}. */
    CHECK(tok[0] == 0x2D && tok[1] == 0x00 && tok[2] == 0x10,
          "got {%02x,%02x,%02x}", tok[0], tok[1], tok[2]);

    current = "token_addr_ep";
    usb_build_token(tok, USB_PID_IN, 0x3A, 0x0A);
    uint16_t field = (uint16_t)(0x3A | (0x0A << 7));
    CHECK(((tok[2] >> 3) & 0x1F) == usb_crc5(field, 11), "crc5 placement");
    CHECK((tok[1] | ((tok[2] & 0x07) << 8)) == field, "field placement");

    current = "data_roundtrip";
    uint8_t payload[18] = {0x12,0x01,0x00,0x02,0,0,0,64,0x11,0x22,0x33,0x44,0x45,0,1,2,3,1};
    uint8_t pkt[64];
    size_t total = usb_build_data(pkt, USB_PID_DATA1, payload, sizeof payload);
    CHECK(total == sizeof payload + 3, "len %zu", total);
    size_t plen = 0;
    CHECK(usb_check_data(pkt, total, &plen) && plen == sizeof payload, "check ok");
    pkt[5] ^= 0xFF;                         /* corrupt one byte */
    CHECK(!usb_check_data(pkt, total, &plen), "corruption detected");
}

/* ---------------- ULPI register-cycle state machine ---------------- */

typedef struct {
    char op[32]; uint8_t val[32]; int n;
    uint8_t read_val;
} fake_bus;

static void fb_cmd(ulpi_io *io, uint8_t c)  { fake_bus *f = io->ctx; f->op[f->n]='C'; f->val[f->n++]=c; }
static void fb_write(ulpi_io *io, uint8_t d){ fake_bus *f = io->ctx; f->op[f->n]='W'; f->val[f->n++]=d; }
static uint8_t fb_read(ulpi_io *io)         { fake_bus *f = io->ctx; f->op[f->n]='R'; f->val[f->n++]=f->read_val; return f->read_val; }
static void fb_turn(ulpi_io *io, int in)    { fake_bus *f = io->ctx; f->op[f->n]='T'; f->val[f->n++]=(uint8_t)in; }
static void fb_stp(ulpi_io *io)             { fake_bus *f = io->ctx; f->op[f->n]='S'; f->val[f->n++]=0; }

static void test_ulpi_regcycle(void)
{
    fake_bus f = {0};
    ulpi_io io = { fb_cmd, fb_write, fb_read, fb_turn, fb_stp, &f };

    current = "reg_write";
    ulpi_reg_write(&io, ULPI_REG_FUNC_CTRL, 0x45);
    /* expect: C 0x84 (0x80|addr), W 0x45, S */
    CHECK(f.n == 3 && f.op[0]=='C' && f.val[0]==0x84 &&
          f.op[1]=='W' && f.val[1]==0x45 && f.op[2]=='S', "write sequence");

    current = "reg_read";
    f.n = 0; f.read_val = 0x41;
    uint8_t v = ulpi_reg_read(&io, ULPI_REG_FUNC_CTRL);
    /* expect: C 0xC4 (0xC0|addr), T in, R, T out ; returns 0x41 */
    CHECK(v == 0x41, "read value 0x%02x", v);
    CHECK(f.n == 4 && f.op[0]=='C' && f.val[0]==0xC4 &&
          f.op[1]=='T' && f.val[1]==1 && f.op[2]=='R' &&
          f.op[3]=='T' && f.val[3]==0, "read sequence");

    current = "phy_init_fs";
    f.n = 0; f.read_val = 0x45;
    v = ulpi_phy_init_fs(&io);
    /* FUNC_CTRL write value = SUSPENDM|TERMSELECT|XCVRSEL_FS = 0x40|0x04|0x01 = 0x45 */
    CHECK(f.op[0]=='C' && f.val[0]==ULPI_TXCMD_REGWRITE(ULPI_REG_FUNC_CTRL) &&
          f.op[1]=='W' && f.val[1]==0x45, "func_ctrl=0x45");
    /* then OTG_CTRL write = 0 */
    CHECK(f.op[3]=='C' && f.val[3]==ULPI_TXCMD_REGWRITE(ULPI_REG_OTG_CTRL) &&
          f.op[4]=='W' && f.val[4]==0x00, "otg_ctrl=0");
    CHECK(v == 0x45, "readback 0x%02x", v);
}

/* ---------------- DCD endpoint / enumeration state machine ---------------- */

/* A minimal "stack": answers SETUP the way TinyUSB's usbd would for the three
   standard requests that enumeration needs. */
static usb_ep_ctx_t C;
static int completed_in, completed_out, address_events;
static uint8_t last_address;

static const uint8_t DEVICE_DESC[18] = {
    18, 0x01, 0x00, 0x02, 0,0,0, 64, 0xCC,0x17, 0x50,0x13, 0x45,0x00, 1,2,3, 1
};

static void mock_setup(void *u, const uint8_t s[8])
{
    (void)u;
    uint8_t bReq = s[1];
    uint16_t wValue  = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t wLength = (uint16_t)(s[6] | (s[7] << 8));
    if (bReq == 6) {                 /* GET_DESCRIPTOR */
        uint16_t n = wLength < sizeof DEVICE_DESC ? wLength : sizeof DEVICE_DESC;
        usb_ep_xfer(&C, 0x80, (uint8_t *)DEVICE_DESC, n);
    } else if (bReq == 5) {          /* SET_ADDRESS */
        usb_ep_set_address(&C, (uint8_t)wValue);
    } else if (bReq == 9) {          /* SET_CONFIGURATION */
        usb_ep_open(&C, 0x81, USB_XFER_BULK, 64);      /* MIDI bulk IN */
        usb_ep_open(&C, 0x01, USB_XFER_BULK, 64);      /* MIDI bulk OUT */
        usb_ep_xfer(&C, 0x80, NULL, 0);                /* ZLP status */
    }
}
static void mock_complete(void *u, uint8_t ep, uint16_t len)
{
    (void)u;
    if (ep & 0x80) {
        completed_in++;
        /* After a control-read data stage, usbd queues the OUT ZLP status. */
        if (ep == 0x80 && len > 0)
            usb_ep_xfer(&C, 0x00, NULL, 0);
    } else {
        completed_out++;
    }
}
static void mock_address(void *u, uint8_t a) { (void)u; address_events++; last_address = a; }

static void setup_ctx(void)
{
    memset(&C, 0, sizeof C);
    usb_ep_init(&C);
    C.on_setup = mock_setup;
    C.on_complete = mock_complete;
    C.on_address = mock_address;
    completed_in = completed_out = address_events = 0;
}

/* Drain an IN data stage into out[], returning the byte count. Checks DATA1
   starts the stage and the toggle alternates. */
static int drain_in(uint8_t epnum, uint8_t *out, int *toggle_ok)
{
    int got = 0, expect = 1;   /* control/data stage begins on DATA1 */
    *toggle_ok = 1;
    for (;;) {
        usb_resp_t r = usb_ep_on_in(&C, epnum);
        if (r.kind != USB_RESP_DATA) break;
        usb_pid_t exp_pid = expect ? USB_PID_DATA1 : USB_PID_DATA0;
        if (r.pid != exp_pid) *toggle_ok = 0;
        expect ^= 1;
        if (out && r.len) memcpy(out + got, r.data, r.len);
        got += r.len;
        if (r.len < 64) break;   /* short packet ends the stage */
    }
    return got;
}

static void test_enumeration(void)
{
    uint8_t buf[64];
    int tog;

    /* 1. GET_DESCRIPTOR(device, wLength=64) */
    current = "get_descriptor";
    setup_ctx();
    uint8_t get_desc[8] = { 0x80, 6, 0x00, 0x01, 0,0, 64,0 };
    usb_ep_on_setup(&C, get_desc);
    int n = drain_in(0, buf, &tog);
    CHECK(n == 18, "descriptor len %d", n);
    CHECK(tog, "DATA1-first toggle");
    CHECK(memcmp(buf, DEVICE_DESC, 18) == 0, "descriptor bytes");
    CHECK(completed_in == 1, "IN completed %d", completed_in);
    /* status OUT ZLP (DATA1) */
    usb_resp_t st = usb_ep_on_out(&C, 0, NULL, 0);
    CHECK(st.kind == USB_RESP_ACK, "status ACK");
    CHECK(completed_out == 1, "OUT status completed");

    /* 2. SET_ADDRESS(7): address must latch only after the status IN completes */
    current = "set_address";
    setup_ctx();
    uint8_t set_addr[8] = { 0x00, 5, 7,0, 0,0, 0,0 };
    usb_ep_on_setup(&C, set_addr);
    CHECK(C.address == 0, "address still 0 before status");
    usb_resp_t z = usb_ep_on_in(&C, 0);         /* EP0-IN ZLP status */
    CHECK(z.kind == USB_RESP_DATA && z.len == 0 && z.pid == USB_PID_DATA1, "ZLP DATA1");
    CHECK(C.address == 7 && last_address == 7 && address_events == 1, "address latched to 7");

    /* 3. SET_CONFIGURATION(1): opens EP1, status ZLP, then bulk IN works */
    current = "set_configuration";
    setup_ctx();
    uint8_t set_cfg[8] = { 0x00, 9, 1,0, 0,0, 0,0 };
    usb_ep_on_setup(&C, set_cfg);
    usb_resp_t cz = usb_ep_on_in(&C, 0);
    CHECK(cz.kind == USB_RESP_DATA && cz.len == 0, "config status ZLP");
    CHECK(C.ep[1][1].configured && C.ep[1][1].max_packet == 64, "EP1 IN open");

    current = "bulk_in_multi_packet";
    static uint8_t big[100];
    for (int i = 0; i < 100; i++) big[i] = (uint8_t)i;
    usb_ep_xfer(&C, 0x81, big, 100);
    usb_resp_t b0 = usb_ep_on_in(&C, 1);
    usb_resp_t b1 = usb_ep_on_in(&C, 1);
    CHECK(b0.kind == USB_RESP_DATA && b0.len == 64 && b0.pid == USB_PID_DATA0, "pkt0 64 DATA0");
    CHECK(b1.kind == USB_RESP_DATA && b1.len == 36 && b1.pid == USB_PID_DATA1, "pkt1 36 DATA1");
    usb_resp_t b2 = usb_ep_on_in(&C, 1);
    CHECK(b2.kind == USB_RESP_NAK, "no more data -> NAK");

    current = "stall";
    usb_ep_stall(&C, 0x81);
    CHECK(usb_ep_on_in(&C, 1).kind == USB_RESP_STALL, "stalled EP returns STALL");
    usb_ep_clear_stall(&C, 0x81);
    CHECK(C.ep[1][1].toggle == 0, "clear_stall resets toggle");
}

int main(void)
{
    test_crc_and_framing();
    test_ulpi_regcycle();
    test_enumeration();
    if (failures == 0)
        printf("test_usb: all passed\n");
    return failures != 0;
}
