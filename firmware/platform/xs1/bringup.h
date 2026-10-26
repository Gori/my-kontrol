#pragma once
/* USB bring-up builds (make BOARD=s49 DEV=1 BRINGUP=1, MK_BRINGUP; tile 1, tile1.c): with
   no host configuring the USB device within BRINGUP_MS of a start, tile 1 writes a
   diagnostic record to the SPI flash log at 0xF5000, removes the upgrade image (as
   LINK_SYS_FACTORY) and reboots, so NI's factory image (working USB and DFU) comes up for
   the next try. Release and other builds contain none of this (xtest bringup checks). */
#include <stdint.h>

#define BRINGUP_MS      20000u
#define BRINGUP_LOG     0xF5000u          /* one 4 KB sector, 16 records */
#define BRINGUP_REC     256u
#define BRINGUP_MAGIC   0x55424B4Du       /* "MKBU" */

/* filled by the SIE thread (usb_ulpi.c) and the DCD (dcd_xs1_ulpi.c) as they go */
typedef struct {
    volatile uint32_t magic;
    volatile uint16_t version, seq;       /* record format 1; the record's number */
    volatile uint32_t uptime_ms;
    volatile uint8_t sie_stage;           /* 1 ports set up, 2 PHY reset written, 3 IDs read,
                                             4 PHY set to full speed, 5 in the SIE loop */
    volatile uint8_t ulpi_id[4];          /* ULPI vendor ID low/high, product ID low/high */
    volatile uint8_t func_ctrl;           /* FUNC_CTRL read back after set-up */
    volatile uint8_t last_rx0;            /* the first byte of the last thing the PHY sent */
    volatile uint8_t flags;               /* 1 bus reset seen, 2 SETUP, 4 address set, 8 configured */
    volatile uint8_t address, pad[3];
    volatile uint32_t wire_events;        /* the SIE's DIR events */
    volatile uint32_t rx_packets, bad_pid, setup_tokens, in_tokens, out_tokens, other_tokens;
    volatile uint32_t evt_reset, evt_setup, evt_complete, dcd_xfers, dcd_stalls;
} bringup_diag_t;

#ifdef MK_BRINGUP
extern bringup_diag_t bringup_diag;
#define BRINGUP_DIAG(x) do { x; } while (0)
/* tile 1 (tile1.c) */
void bringup_start(void);                 /* main, before USB: the log into RAM, uptime base */
int bringup_due(void);                    /* the USB thread: 20 s and never configured */
void bringup_revert(void);                /* main: record, 5-pin report, revert, reboot */
void bringup_report(void);                /* main: the last record out on the 5-pin OUT */
const uint8_t *bringup_record(uint8_t slot);   /* the log in RAM (vendor request 0xB9) */
#else
#define BRINGUP_DIAG(x) do { } while (0)
#endif
