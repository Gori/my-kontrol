/* XS1 ULPI link: ports, clock block, PHY bring-up, packet TX/RX and the SIE
   thread. Clean-room from the ULPI 1.1 and XS1 architecture manuals; no XMOS or
   NI source. Compiled only for xcore; the pure logic it calls (usb_packet.c,
   usb_ep.c, usb_crc.c) is what the host test exercises.

   HARDWARE-UNVERIFIED: there is no USB model in the emulator and no board here,
   so none of the port/clock/channel code below has been run. The XS1 setc
   control-token values, the port resource ids (usb_ulpi.h) and the 60 MHz
   packet timing are bring-up items — see usb.md "Risks". */
#include "usb_ulpi.h"
#include "usb_packet.h"
#include "usb_ep.h"

#if defined(__xcore__)

/* ---- XS1 setc values (as in xs1.h: mode << 3 | 7, or 8 for INUSE_ON) ---- */
#define SC_ENABLE        0x0008u   /* INUSE_ON: claim the resource            */
#define SC_BUF           0x200Fu   /* BUF_BUFFERS: buffered port              */
#define SC_RDY_HANDSHAKE 0x3017u   /* RDY_HANDSHAKE: port gated by ready-in   */
#define SC_PORT_CLOCK    0x500Fu   /* PORT_CLOCKPORT: use port as clock input */
#define SC_EVENT         0x0002u   /* IE_MODE_EVENT                           */
#define SC_START         0x000Fu   /* RUN_STARTR: start a clock block         */

/* ---- resource primitives (one inline-asm op each) ---- */
static inline void r_setc(uint32_t r, uint32_t v)   { asm volatile("setc res[%0], %1"  :: "r"(r), "r"(v) : "memory"); }
static inline void r_setclk(uint32_t r, uint32_t c) { asm volatile("setclk res[%0], %1":: "r"(r), "r"(c) : "memory"); }
static inline void r_setrdy(uint32_t r, uint32_t s) { asm volatile("setrdy res[%0], %1":: "r"(r), "r"(s) : "memory"); }
static inline void r_settw(uint32_t r, uint32_t w)  { asm volatile("settw res[%0], %1" :: "r"(r), "r"(w) : "memory"); }
static inline void p_out(uint32_t p, uint32_t v)    { asm volatile("out res[%0], %1"   :: "r"(p), "r"(v) : "memory"); }
static inline uint32_t p_in(uint32_t p)             { uint32_t v; asm volatile("in %0, res[%1]"    : "=r"(v) : "r"(p) : "memory"); return v; }

static inline uint32_t c_int(uint32_t c)            { uint32_t t; asm volatile("int %0, res[%1]" : "=r"(t) : "r"(c) : "memory"); return t; }

/* The data bus is turned by claiming/releasing drive. On a shared 8-bit port,
   output drive is active after out; reading needs drive released first. The
   exact mechanism (and whether a second 8-bit port is needed to avoid a dead
   ULPI cycle on turnaround) is the top bring-up risk. */
static inline void data_drive(int on) { r_setc(ULPI_PORT_DATA, on ? SC_ENABLE : SC_ENABLE); }

/* ---- real ULPI register bus (used only at init, so polling NXT is fine) ---- */
static void io_cmd(ulpi_io *io, uint8_t txcmd)
{
    (void)io;
    data_drive(1);
    while (!(p_in(ULPI_PORT_DIR) & 1)) { }   /* DIR must be low to drive */
    p_out(ULPI_PORT_DATA, txcmd);
    while (!(p_in(ULPI_PORT_NXT) & 1)) { }   /* PHY latched the CMD */
}
static void io_write(ulpi_io *io, uint8_t data)
{
    (void)io;
    p_out(ULPI_PORT_DATA, data);
    while (!(p_in(ULPI_PORT_NXT) & 1)) { }
}
static uint8_t io_read(ulpi_io *io)
{
    (void)io;
    return (uint8_t)p_in(ULPI_PORT_DATA);
}
static void io_turn(ulpi_io *io, int to_input)
{
    (void)io;
    data_drive(!to_input);
}
static void io_stp(ulpi_io *io)
{
    (void)io;
    p_out(ULPI_PORT_STP, 1);
    p_out(ULPI_PORT_STP, 0);
}

static ulpi_io g_io = { io_cmd, io_write, io_read, io_turn, io_stp, 0 };

/* ---- port + clock-block setup ---- */
static void ports_init(void)
{
    /* Clock block sourced from the 60 MHz ULPI CLK on 1H. */
    r_setc(ULPI_CLKBLK, SC_ENABLE);
    r_setc(ULPI_PORT_CLK, SC_ENABLE);
    r_setclk(ULPI_CLKBLK, ULPI_PORT_CLK);     /* clock block <- 1H */

    r_setc(ULPI_PORT_DATA, SC_ENABLE);
    r_setc(ULPI_PORT_DATA, SC_BUF);           /* buffered 8-bit */
    r_settw(ULPI_PORT_DATA, 8);
    r_setclk(ULPI_PORT_DATA, ULPI_CLKBLK);

    r_setc(ULPI_PORT_DIR, SC_ENABLE); r_setclk(ULPI_PORT_DIR, ULPI_CLKBLK);
    r_setc(ULPI_PORT_NXT, SC_ENABLE); r_setclk(ULPI_PORT_NXT, ULPI_CLKBLK);
    r_setc(ULPI_PORT_STP, SC_ENABLE); r_setclk(ULPI_PORT_STP, ULPI_CLKBLK);

    /* NXT gates the data port's buffered transfers during a packet. */
    r_setrdy(ULPI_PORT_DATA, ULPI_PORT_NXT);
    r_setc(ULPI_PORT_DATA, SC_RDY_HANDSHAKE);

    r_setc(ULPI_CLKBLK, SC_START);             /* start the clock block */
}

uint8_t usb_ulpi_phy_bringup(void)
{
    ports_init();
    /* FUNC_CTRL.RESET pulse, then full-speed device config. The PHY must be
       supplying CLK already (external PHY drives the link clock on 1H). */
    ulpi_reg_write(&g_io, ULPI_REG_FUNC_CTRL, ULPI_FC_RESET);
    for (volatile int i = 0; i < 1000; i++) { }
    return ulpi_phy_init_fs(&g_io);            /* FUNC_CTRL = FS + TERMSELECT + SUSPENDM */
}

/* ---- packet TX/RX over the data port (60 MHz; HARDWARE-UNVERIFIED) ----
   ULPI 1.1 §3.8.1: a USB transmit is a TX CMD byte (01xx | PID) followed by the
   packet bytes *after* the PID, ended by STP. On receive the PHY drives DIR and
   streams the packet starting with the PID byte. */
static void txpkt(usb_pid_t pid, const uint8_t *after_pid, int len)
{
    data_drive(1);
    while (p_in(ULPI_PORT_DIR) & 1) { }        /* wait for the bus */
    p_out(ULPI_PORT_DATA, ULPI_TXCMD_TXPID(pid));
    while (!(p_in(ULPI_PORT_NXT) & 1)) { }
    for (int i = 0; i < len; i++) {
        p_out(ULPI_PORT_DATA, after_pid[i]);
        while (!(p_in(ULPI_PORT_NXT) & 1)) { }
    }
    io_stp(&g_io);
}

static int rxpkt(uint8_t *buf, int max)
{
    int n = 0;
    data_drive(0);
    while (!(p_in(ULPI_PORT_DIR) & 1)) { }     /* PHY took the bus */
    while ((p_in(ULPI_PORT_DIR) & 1) && n < max) {
        if (p_in(ULPI_PORT_NXT) & 1)           /* NXT marks a real data byte */
            buf[n++] = (uint8_t)p_in(ULPI_PORT_DATA);
    }
    return n;
}

/* ---- the SIE: owns usb_ep, drives the wire, talks to the DCD over `chan` ---- */
static usb_ep_ctx_t g_ep;
static uint32_t     g_chan;

static void ev_setup(void *u, const uint8_t s[8])
{
    (void)u;
    uint32_t a = s[0] | s[1]<<8 | s[2]<<16 | (uint32_t)s[3]<<24;
    uint32_t b = s[4] | s[5]<<8 | s[6]<<16 | (uint32_t)s[7]<<24;
    ulpi_put(&ulpi_evq, g_chan, ULPI_EVT_SETUP, a, b, 0);
}
static void ev_complete(void *u, uint8_t ep, uint16_t len)
{
    (void)u;
    ulpi_put(&ulpi_evq, g_chan, ULPI_EVT_COMPLETE, ep, len, 0);
}
static void ev_address(void *u, uint8_t addr) { (void)u; (void)addr; /* filter already updated in g_ep */ }

/* Apply one DCD command word-set to g_ep. */
static void apply_cmd(uint32_t tag, uint32_t a, uint32_t b, uint32_t cc)
{
    switch (tag) {
    case ULPI_CMD_OPEN:      usb_ep_open(&g_ep, (uint8_t)a, (usb_xfer_type_t)b, (uint16_t)cc); break;
    case ULPI_CMD_XFER:      usb_ep_xfer(&g_ep, (uint8_t)a, (uint8_t *)b, (uint16_t)cc); break;
    case ULPI_CMD_STALL:     usb_ep_stall(&g_ep, (uint8_t)a); break;
    case ULPI_CMD_CLR_STALL: usb_ep_clear_stall(&g_ep, (uint8_t)a); break;
    case ULPI_CMD_SET_ADDR:  usb_ep_set_address(&g_ep, (uint8_t)a); break;
    case ULPI_CMD_CLOSE_ALL: usb_ep_close_all(&g_ep); break;
    default: break;
    }
}

/* Handle one received USB token + its data/handshake phase. */
static void service_wire(void)
{
    uint8_t buf[72];
    int n = rxpkt(buf, sizeof buf);
    if (n < 1 || !usb_pid_valid(buf[0]))
        return;
    usb_pid_t pid = usb_pid_of(buf[0]);
    uint8_t ep = (uint8_t)((buf[1] >> 7) | ((buf[2] & 0x07) << 1));   /* from the token */

    if (pid == USB_PID_SETUP) {
        uint8_t data[72];
        int dn = rxpkt(data, sizeof data);       /* the DATA0 with the 8 setup bytes */
        if (dn >= 1 + 8)
            usb_ep_on_setup(&g_ep, data + 1);
        txpkt(USB_PID_ACK, 0, 0);
    } else if (pid == USB_PID_IN) {
        usb_resp_t r = usb_ep_on_in(&g_ep, ep);
        if (r.kind == USB_RESP_DATA)      txpkt(r.pid, r.data, r.len);
        else if (r.kind == USB_RESP_STALL) txpkt(USB_PID_STALL, 0, 0);
        else                               txpkt(USB_PID_NAK, 0, 0);
    } else if (pid == USB_PID_OUT) {
        uint8_t data[72];
        int dn = rxpkt(data, sizeof data);
        size_t plen = 0;
        if (dn >= 3 && usb_check_data(data, dn, &plen)) {
            usb_resp_t r = usb_ep_on_out(&g_ep, ep, data + 1, (uint16_t)plen);
            txpkt(r.kind == USB_RESP_STALL ? USB_PID_STALL :
                  r.kind == USB_RESP_NAK   ? USB_PID_NAK   : USB_PID_ACK, 0, 0);
        }
    }
}

/* Block until either the channel or the wire (DIR edge) fires. Returns
   0 = channel, 1 = wire. All event asm is isolated here. HARDWARE-UNVERIFIED. */
static uint32_t wait_event(uint32_t chan, uint32_t dirport)
{
    uint32_t which;
    asm volatile(
        "  setc res[%1], %3        \n"   /* chanend  -> event mode */
        "  setc res[%2], %3        \n"   /* DIR port -> event mode */
        "  ldap r11, .Lusb_ev_chan \n"
        "  setv res[%1], r11       \n"
        "  ldap r11, .Lusb_ev_wire \n"
        "  setv res[%2], r11       \n"
        "  eeu res[%1]             \n"
        "  eeu res[%2]             \n"
        "  waiteu                  \n"
        ".Lusb_ev_chan:  ldc %0, 0 \n"
        "  bu .Lusb_ev_done        \n"
        ".Lusb_ev_wire:  ldc %0, 1 \n"
        ".Lusb_ev_done:  edu res[%1]\n"
        "  edu res[%2]             \n"
        : "=r"(which) : "r"(chan), "r"(dirport), "r"(SC_EVENT)
        : "r11", "memory");
    return which;
}

void usb_ulpi_sie_thread(uint32_t chan)
{
    g_chan = chan;
    usb_ep_init(&g_ep);
    g_ep.on_setup    = ev_setup;
    g_ep.on_complete = ev_complete;
    g_ep.on_address  = ev_address;

    usb_ulpi_phy_bringup();

    for (;;) {
        if (wait_event(chan, ULPI_PORT_DIR) == 0) {
            ulpi_msg_t m;
            (void)c_int(chan);
            ulpi_cmdq.bell = 0;
            while (ulpi_get(&ulpi_cmdq, &m))
                apply_cmd(m.tag, m.a, m.b, m.c);
        } else {
            service_wire();
        }
    }
}

#endif /* __xcore__ */
