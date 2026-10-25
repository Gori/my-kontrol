/* USB 2.0 packet framing and the ULPI 1.1 register-cycle sequencer.
   Pure logic: every function here produces or consumes plain byte buffers, or
   drives an abstract ULPI bus (struct ulpi_io). No XS1 ports, so this file
   builds on the host and is unit-tested against a fake bus (firmware/test).

   What the PHY does for us (ULPI 1.1 §3): NRZI, bit-stuffing, SYNC, EOP and the
   CRC *bits* on the wire are the PHY's job. We provide the packet bytes: PID,
   token/data fields, CRC5/CRC16, and we sequence the ULPI TX CMD / turnaround /
   RX byte stream. */
#ifndef USB_PACKET_H
#define USB_PACKET_H

#include <stddef.h>
#include <stdint.h>

/* --- PIDs (USB 2.0 §8.3.1). On the ULPI data bus the PID is one byte:
   low nibble = PID, high nibble = ~PID. --- */
typedef enum {
    USB_PID_OUT   = 0x1, USB_PID_IN    = 0x9, USB_PID_SOF  = 0x5, USB_PID_SETUP = 0xD,
    USB_PID_DATA0 = 0x3, USB_PID_DATA1 = 0xB, USB_PID_DATA2 = 0x7, USB_PID_MDATA = 0xF,
    USB_PID_ACK   = 0x2, USB_PID_NAK   = 0xA, USB_PID_STALL = 0xE, USB_PID_NYET  = 0x6,
} usb_pid_t;

/* Encode/validate the one-byte on-wire PID. */
uint8_t usb_pid_byte(usb_pid_t pid);
int     usb_pid_valid(uint8_t b);          /* 1 if high nibble == ~low nibble */
usb_pid_t usb_pid_of(uint8_t b);           /* low nibble */

/* Build a token packet (SETUP/IN/OUT/SOF) into out[3]: PID byte then the
   11-bit addr/endp field with CRC5, low byte first. Returns bytes written (3). */
size_t usb_build_token(uint8_t out[3], usb_pid_t pid, uint8_t addr, uint8_t endp);

/* Build a data packet: PID, payload, CRC16 (low byte first). out must hold
   1 + len + 2 bytes. Returns total length. */
size_t usb_build_data(uint8_t *out, usb_pid_t data_pid,
                      const uint8_t *payload, size_t len);

/* Validate a received data packet (buf[0] = PID .. last 2 = CRC16). Returns
   payload length via *payload_len and 1 if PID and CRC16 are intact. */
int usb_check_data(const uint8_t *buf, size_t total, size_t *payload_len);

/* --- ULPI register access (ULPI 1.1 §3.8). Abstract one-byte bus so the
   command/turnaround sequence can be driven by a fake in the host test. On
   real silicon usb_ulpi.c implements these over XS1 buffered ports. --- */
typedef struct ulpi_io {
    /* Drive a TX CMD byte onto DATA[7:0] and wait for the PHY's NXT. */
    void    (*cmd)(struct ulpi_io *io, uint8_t txcmd);
    /* Drive one data byte while NXT is asserted. */
    void    (*write)(struct ulpi_io *io, uint8_t data);
    /* Sample DATA[7:0] (PHY is driving, DIR high). */
    uint8_t (*read)(struct ulpi_io *io);
    /* Turn the data port: to_input != 0 hands the bus to the PHY. */
    void    (*turn)(struct ulpi_io *io, int to_input);
    /* Drive STP for one cycle to end a TX. */
    void    (*stp)(struct ulpi_io *io);
    void    *ctx;
} ulpi_io;

/* ULPI TX CMD byte (ULPI 1.1 §3.8.1.1/§3.8.2.1):
   register write = 10 AAAAAA, register read = 11 AAAAAA, NOOP = 00000000,
   transmit USB packet = 01 00 PPPP (PPPP = PID). */
#define ULPI_TXCMD_REGWRITE(a) (uint8_t)(0x80 | ((a) & 0x3F))
#define ULPI_TXCMD_REGREAD(a)  (uint8_t)(0xC0 | ((a) & 0x3F))
#define ULPI_TXCMD_NOOP        0x00u
#define ULPI_TXCMD_TXPID(p)    (uint8_t)(0x40 | ((p) & 0x0F))

/* Immediate register write / read (ULPI 1.1 §3.8.2). */
void    ulpi_reg_write(ulpi_io *io, uint8_t addr, uint8_t value);
uint8_t ulpi_reg_read(ulpi_io *io, uint8_t addr);

/* ULPI register addresses used at init (ULPI 1.1 §4). */
#define ULPI_REG_FUNC_CTRL      0x04
#define ULPI_REG_FUNC_CTRL_SET  0x05
#define ULPI_REG_FUNC_CTRL_CLR  0x06
#define ULPI_REG_OTG_CTRL       0x0A
#define ULPI_REG_OTG_CTRL_SET   0x0B
#define ULPI_REG_USB_INT_EN_R   0x0D   /* rising-edge int enable */
#define ULPI_REG_USB_INT_EN_F   0x10   /* falling-edge int enable */

/* FUNC_CTRL bits (ULPI 1.1 §4.1). */
#define ULPI_FC_XCVRSEL_HS    0x0      /* high speed */
#define ULPI_FC_XCVRSEL_FS    0x1      /* full speed  <- our target */
#define ULPI_FC_XCVRSEL_LS    0x2
#define ULPI_FC_TERMSELECT    0x04     /* enable FS/LS termination */
#define ULPI_FC_OPMODE_NORMAL 0x00
#define ULPI_FC_RESET         0x20     /* PHY reset */
#define ULPI_FC_SUSPENDM      0x40     /* 0 = suspend, 1 = powered */

/* OTG_CTRL bits (ULPI 1.1 §4.2). */
#define ULPI_OTG_DP_PULLDOWN  0x02
#define ULPI_OTG_DM_PULLDOWN  0x04

/* Full-speed device init: FUNC_CTRL = SUSPENDM | TERMSELECT | XCVRSEL_FS,
   OTG_CTRL = 0 (device, no pulldowns). Returns the FUNC_CTRL read back. */
uint8_t ulpi_phy_init_fs(ulpi_io *io);

#endif
